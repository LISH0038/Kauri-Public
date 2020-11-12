/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hotstuff/hotstuff.h"

#include <random>
#include "hotstuff/client.h"
#include "hotstuff/liveness.h"

using salticidae::static_pointer_cast;

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN

namespace hotstuff {

const opcode_t MsgPropose::opcode;
MsgPropose::MsgPropose(const Proposal &proposal) { serialized << proposal; }
void MsgPropose::postponed_parse(HotStuffCore *hsc) {
    proposal.hsc = hsc;
    serialized >> proposal;
}

const opcode_t MsgRelay::opcode;
MsgRelay::MsgRelay(const VoteRelay &proposal) { serialized << proposal; }
void MsgRelay::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgVote::opcode;
MsgVote::MsgVote(const Vote &vote) { serialized << vote; }
void MsgVote::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgReqBlock::opcode;
MsgReqBlock::MsgReqBlock(const std::vector<uint256_t> &blk_hashes) {
    serialized << htole((uint32_t)blk_hashes.size());
    for (const auto &h: blk_hashes)
        serialized << h;
}

MsgReqBlock::MsgReqBlock(DataStream &&s) {
    uint32_t size;
    s >> size;
    size = letoh(size);
    blk_hashes.resize(size);
    for (auto &h: blk_hashes) s >> h;
}

const opcode_t MsgRespBlock::opcode;
MsgRespBlock::MsgRespBlock(const std::vector<block_t> &blks) {
    serialized << htole((uint32_t)blks.size());
    for (auto blk: blks) serialized << *blk;
}

void MsgRespBlock::postponed_parse(HotStuffCore *hsc) {
    uint32_t size;
    serialized >> size;
    size = letoh(size);
    blks.resize(size);
    for (auto &blk: blks)
    {
        Block _blk;
        _blk.unserialize(serialized, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    }
}

void HotStuffBase::exec_command(uint256_t cmd_hash, commit_cb_t callback) {
    cmd_pending.enqueue(std::make_pair(cmd_hash, callback));
}

void HotStuffBase::on_fetch_blk(const block_t &blk) {
#ifdef HOTSTUFF_BLK_PROFILE
    blk_profiler.get_tx(blk->get_hash());
#endif
    LOG_DEBUG("fetched %.10s", get_hex(blk->get_hash()).c_str());
    part_fetched++;
    fetched++;
    //for (auto cmd: blk->get_cmds()) on_fetch_cmd(cmd);
    const uint256_t &blk_hash = blk->get_hash();
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it != blk_fetch_waiting.end())
    {
        it->second.resolve(blk);
        blk_fetch_waiting.erase(it);
    }
}

bool HotStuffBase::on_deliver_blk(const block_t &blk) {
    const uint256_t &blk_hash = blk->get_hash();
    bool valid;
    /* sanity check: all parents must be delivered */
    for (const auto &p: blk->get_parent_hashes())
        assert(storage->is_blk_delivered(p));
    if ((valid = HotStuffCore::on_deliver_blk(blk)))
    {
        LOG_DEBUG("block %.10s delivered",
                get_hex(blk_hash).c_str());
        part_parent_size += blk->get_parent_hashes().size();
        part_delivered++;
        delivered++;
    }
    else
    {
        LOG_WARN("dropping invalid block");
    }

    bool res = true;
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
    {
        auto &pm = it->second;
        if (valid)
        {
            pm.elapsed.stop(false);
            auto sec = pm.elapsed.elapsed_sec;
            part_delivery_time += sec;
            part_delivery_time_min = std::min(part_delivery_time_min, sec);
            part_delivery_time_max = std::max(part_delivery_time_max, sec);

            pm.resolve(blk);
        }
        else
        {
            pm.reject(blk);
            res = false;
        }
        blk_delivery_waiting.erase(it);
    }
    return res;
}

promise_t HotStuffBase::async_fetch_blk(const uint256_t &blk_hash,
                                        const PeerId *replica,
                                        bool fetch_now) {
    if (storage->is_blk_fetched(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm){
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it == blk_fetch_waiting.end())
    {
#ifdef HOTSTUFF_BLK_PROFILE
        blk_profiler.rec_tx(blk_hash, false);
#endif
        it = blk_fetch_waiting.insert(
            std::make_pair(
                blk_hash,
                BlockFetchContext(blk_hash, this))).first;
    }
    if (replica != nullptr)
        it->second.add_replica(*replica, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_deliver_blk(const uint256_t &blk_hash, const PeerId &replica) {
    if (storage->is_blk_delivered(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm) {
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
        return static_cast<promise_t &>(it->second);
    BlockDeliveryContext pm{[](promise_t){}};
    it = blk_delivery_waiting.insert(std::make_pair(blk_hash, pm)).first;
    /* otherwise the on_deliver_batch will resolve */
    async_fetch_blk(blk_hash, &replica).then([this, replica](block_t blk) {
        /* qc_ref should be fetched */
        std::vector<promise_t> pms;
        const auto &qc = blk->get_qc();
        assert(qc);
        if (blk == get_genesis())
            pms.push_back(promise_t([](promise_t &pm){ pm.resolve(true); }));
        else
            pms.push_back(blk->verify(this, vpool));
        pms.push_back(async_fetch_blk(qc->get_obj_hash(), &replica));
        /* the parents should be delivered */
        for (const auto &phash: blk->get_parent_hashes())
            pms.push_back(async_deliver_blk(phash, replica));
        promise::all(pms).then([this, blk](const promise::values_t values) {
            auto ret = promise::any_cast<bool>(values[0]) && this->on_deliver_blk(blk);
            if (!ret)
                HOTSTUFF_LOG_WARN("verification failed during async delivery");
        });
    });
    return static_cast<promise_t &>(pm);
}

void HotStuffBase::propose_handler(MsgPropose &&msg, const Net::conn_t &conn) {
    //std::cout << "Propose handler" << std::endl;
    const PeerId &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    auto &prop = msg.proposal;

    block_t blk = prop.blk;
    if (!blk) return;

    //std::cout << "Verify proposal" << std::endl;
    for (const PeerId& peerId : childPeers)
    {
        //std::cout << "Relay proposal" << std::endl;
        pn.send_msg(MsgPropose(prop), peerId);
        //todo this happens to quickly. What do we do then? Just add it ourselves? (try?)
    }

    promise::all(std::vector<promise_t>{
        async_deliver_blk(blk->get_hash(), peer)
    }).then([this, prop = std::move(prop)]() {
        on_receive_proposal(prop);
    });
}

void HotStuffBase::vote_handler(MsgVote &&msg, const Net::conn_t &conn) {

    struct timeval timeStart,
            timeEnd;
    gettimeofday(&timeStart, NULL);

    const auto &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    //std::cout << "vote handler0: " << std::endl;
    block_t blk = get_potentially_not_delivered_blk(msg.vote.blk_hash);
    if (!blk->delivered && blk->self_qc == nullptr) {
        blk->self_qc = create_quorum_cert(blk->get_hash());
        part_cert_bt part = create_part_cert(*priv_key, blk->get_hash());
        blk->self_qc->add_part(config, id, *part);

        std::cout << "create cert: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
    }

    if (id == 0) {
        struct timeval time;
        gettimeofday(&time, NULL);
        std::cout << "vote handler: " << msg.vote.blk_hash.to_hex() << " " << time.tv_sec << std::endl;
    } else {
        std::cout << "vote handler: " << msg.vote.blk_hash.to_hex() << " " << std::endl;
    }

    if (blk->self_qc->has_n(config.nmajority)) {
        //std::cout << "bye vote handler: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
        return;
    }

    if (id != 0 ) {
        auto &cert = blk->self_qc;
        cert->add_part(config, msg.vote.voter, *msg.vote.cert);

        if (!cert->has_n(numberOfChildren + 1)) return;
        cert->compute();
        if (!cert->verify(config)) {
            throw std::runtime_error("Invalid Sigs in intermediate signature!");
        }
        //std::cout << peers[id].to_hex() <<  " send relay message: " << v->blk_hash.to_hex() <<  std::endl;
        pn.send_msg(MsgRelay(VoteRelay(msg.vote.blk_hash, blk->self_qc->clone(), this)), parentPeer);
        async_deliver_blk(msg.vote.blk_hash, peer);
        return;
    }

    //auto &vote = msg.vote;
    RcObj<Vote> v(new Vote(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(v->blk_hash, peer),
        id == 0 ? v->verify(vpool) : promise_t([](promise_t &pm) { pm.resolve(true); }),
    }).then([this, blk, v=std::move(v), timeStart](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid vote from %d", v->voter);
        auto &cert = blk->self_qc;

        cert->add_part(config, v->voter, *v->cert);
        if (cert != nullptr && cert->get_obj_hash() == blk->get_hash()) {
            if (cert->has_n(config.nmajority)) {
                cert->compute();
                if (id != 0 && !cert->verify(config)) {
                    throw std::runtime_error("Invalid Sigs in intermediate signature!");
                }
                update_hqc(blk, cert);
                on_qc_finish(blk);
            }
        }

        struct timeval timeEnd;
        gettimeofday(&timeEnd, NULL);

        std::cout << "Vote handling cost partially threaded: "
                  << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
                  << " us to execute."
                  << std::endl;

    });

    gettimeofday(&timeEnd, NULL);

    std::cout << "Vote handling cost: "
              << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
              << " us to execute."
              << std::endl;
}

void HotStuffBase::vote_relay_handler(MsgRelay &&msg, const Net::conn_t &conn) {
    struct timeval timeStart,
            timeEnd;
    gettimeofday(&timeStart, NULL);

    const auto &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    //std::cout << "vote relay handler: " << msg.vote.blk_hash.to_hex() << std::endl;

    block_t blk = get_potentially_not_delivered_blk(msg.vote.blk_hash);
    if (!blk->delivered && blk->self_qc == nullptr) {
        blk->self_qc = create_quorum_cert(blk->get_hash());
        part_cert_bt part = create_part_cert(*priv_key, blk->get_hash());
        blk->self_qc->add_part(config, id, *part);

        std::cout << "create cert: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
    }

    if (blk->self_qc->has_n(config.nmajority)) {
        std::cout << "bye vote relay handler: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
        return;
    }

    if (id == 0) {
        struct timeval time;
        gettimeofday(&time, NULL);
        std::cout << "vote relay handler: " << msg.vote.blk_hash.to_hex() << " " << time.tv_sec << std::endl;
    } else {
        std::cout << "vote relay handler: " << msg.vote.blk_hash.to_hex() << " " << std::endl;
    }

    //auto &vote = msg.vote;
    RcObj<VoteRelay> v(new VoteRelay(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
            async_deliver_blk(v->blk_hash, peer),
            promise_t([](promise_t &pm) { pm.resolve(true); }), //v->cert->verify(config, vpool),
    }).then([this, blk, v=std::move(v), timeStart](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN ("invalid vote-relay");
        auto &cert = blk->self_qc;
        std::cout << "got relay and verified" << std::endl;

        if (cert != nullptr && cert->get_obj_hash() == blk->get_hash() && !cert->has_n(config.nmajority)) {
            cert->merge_quorum(*v->cert);

            std::cout << "merge quorum" << std::endl;
            if (id != 0) {
                if (!cert->has_n(numberOfChildren + 1)) return;
                cert->compute();
                if (!cert->verify(config)) {
                    throw std::runtime_error("Invalid Sigs in intermediate signature!");
                }
                std::cout << "Send Vote Relay: " << v->blk_hash.to_hex() << std::endl;
                pn.send_msg(MsgRelay(VoteRelay(v->blk_hash, cert.get()->clone(), this)), parentPeer);
                return;
            }

            HOTSTUFF_LOG_PROTO("got %s", std::string(*v).c_str());

            if (!cert->has_n(config.nmajority)) return;

            std::cout << "go to town: " << std::endl;

            cert->compute();
            if (!cert->verify(config)) {
                throw std::runtime_error("Invalid Sigs in intermediate signature!");
            }

            update_hqc(blk, cert);
            on_qc_finish(blk);

            struct timeval timeEnd;
            gettimeofday(&timeEnd, NULL);

            std::cout << "Vote relay handling cost partially threaded: "
                      << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
                      << " us to execute."
                      << std::endl;
        }
    });

    gettimeofday(&timeEnd, NULL);

    std::cout << "Vote relay handling cost: "
              << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
              << " us to execute."
              << std::endl;
}

void HotStuffBase::req_blk_handler(MsgReqBlock &&msg, const Net::conn_t &conn) {
    const PeerId replica = conn->get_peer_id();
    if (replica.is_null()) return;
    auto &blk_hashes = msg.blk_hashes;
    std::vector<promise_t> pms;
    for (const auto &h: blk_hashes)
        pms.push_back(async_fetch_blk(h, nullptr));
    promise::all(pms).then([replica, this](const promise::values_t values) {
        std::vector<block_t> blks;
        for (auto &v: values)
        {
            auto blk = promise::any_cast<block_t>(v);
            blks.push_back(blk);
        }
        pn.send_msg(MsgRespBlock(blks), replica);
    });
}

void HotStuffBase::resp_blk_handler(MsgRespBlock &&msg, const Net::conn_t &) {
    msg.postponed_parse(this);
    for (const auto &blk: msg.blks)
        if (blk) on_fetch_blk(blk);
}

bool HotStuffBase::conn_handler(const salticidae::ConnPool::conn_t &conn, bool connected) {
    if (connected)
    {
        auto cert = conn->get_peer_cert();
        //SALTICIDAE_LOG_INFO("%s", salticidae::get_hash(cert->get_der()).to_hex().c_str());
        return (!cert) || valid_tls_certs.count(salticidae::get_hash(cert->get_der()));
    }
    return true;
}

void HotStuffBase::print_stat() const {
    LOG_INFO("===== begin stats =====");
    LOG_INFO("-------- queues -------");
    LOG_INFO("blk_fetch_waiting: %lu", blk_fetch_waiting.size());
    LOG_INFO("blk_delivery_waiting: %lu", blk_delivery_waiting.size());
    LOG_INFO("decision_waiting: %lu", decision_waiting.size());
    LOG_INFO("-------- misc ---------");
    LOG_INFO("fetched: %lu", fetched);
    LOG_INFO("delivered: %lu", delivered);
    LOG_INFO("cmd_cache: %lu", storage->get_cmd_cache_size());
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    LOG_INFO("------ misc (10s) -----");
    LOG_INFO("fetched: %lu", part_fetched);
    LOG_INFO("delivered: %lu", part_delivered);
    LOG_INFO("decided: %lu", part_decided);
    LOG_INFO("gened: %lu", part_gened);
    LOG_INFO("avg. parent_size: %.3f",
            part_delivered ? part_parent_size / double(part_delivered) : 0);
    LOG_INFO("delivery time: %.3f avg, %.3f min, %.3f max",
            part_delivered ? part_delivery_time / double(part_delivered) : 0,
            part_delivery_time_min == double_inf ? 0 : part_delivery_time_min,
            part_delivery_time_max);

    part_parent_size = 0;
    part_fetched = 0;
    part_delivered = 0;
    part_decided = 0;
    part_gened = 0;
    part_delivery_time = 0;
    part_delivery_time_min = double_inf;
    part_delivery_time_max = 0;
#ifdef HOTSTUFF_MSG_STAT
    LOG_INFO("--- replica msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &replica: peers)
    {
        try {
            auto conn = pn.get_peer_conn(replica);
            if (conn == nullptr) continue;
            size_t ns = conn->get_nsent();
            size_t nr = conn->get_nrecv();
            size_t nsb = conn->get_nsentb();
            size_t nrb = conn->get_nrecvb();
            conn->clear_msgstat();
            LOG_INFO("%s: %u(%u), %u(%u), %u",
                     get_hex10(replica).c_str(), ns, nsb, nr, nrb, part_fetched_replica[replica]);
            _nsent += ns;
            _nrecv += nr;
            part_fetched_replica[replica] = 0;
        }
        catch (...) { }
    }
    nsent += _nsent;
    nrecv += _nrecv;
    LOG_INFO("sent: %lu", _nsent);
    LOG_INFO("recv: %lu", _nrecv);
    LOG_INFO("--- replica msg. total ---");
    LOG_INFO("sent: %lu", nsent);
    LOG_INFO("recv: %lu", nrecv);
#endif
    LOG_INFO("====== end stats ======");
}

HotStuffBase::HotStuffBase(uint32_t blk_size,
                    ReplicaID rid,
                    privkey_bt &&priv_key,
                    NetAddr listen_addr,
                    pacemaker_bt pmaker,
                    EventContext ec,
                    size_t nworker,
                    const Net::Config &netconfig):
        HotStuffCore(rid, std::move(priv_key)),
        listen_addr(listen_addr),
        blk_size(blk_size),
        ec(ec),
        tcall(ec),
        vpool(ec, nworker),
        pn(ec, netconfig),
        pmaker(std::move(pmaker)),

        fetched(0), delivered(0),
        nsent(0), nrecv(0),
        part_parent_size(0),
        part_fetched(0),
        part_delivered(0),
        part_decided(0),
        part_gened(0),
        part_delivery_time(0),
        part_delivery_time_min(double_inf),
        part_delivery_time_max(0)
{
    /* register the handlers for msg from replicas */
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::propose_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::req_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::resp_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_relay_handler, this, _1, _2));
    pn.reg_conn_handler(salticidae::generic_bind(&HotStuffBase::conn_handler, this, _1, _2));
    pn.start();
    pn.listen(listen_addr);
}

void HotStuffBase::do_broadcast_proposal(const Proposal &prop) {
    //MsgPropose prop_msg(prop);
    //pn.multicast_msg(MsgPropose(prop), peers);
    for (const PeerId& peerId : childPeers)
    {
        //std::cout << "send proposal" << std::endl;
        pn.send_msg(MsgPropose(prop), peerId);
    }
}

void HotStuffBase::do_vote(Proposal prop, const Vote &vote) {
    //std::cout << "Create cert and add vote1" << std::endl;

    pmaker->beat_resp(prop.proposer).then([this, vote, prop](ReplicaID proposer) {

        if (proposer == get_id())
        {
            throw HotStuffError("unreachable line");
        }

        if (childPeers.empty()) {
            //std::cout << "send vote" << std::endl;
            pn.send_msg(MsgVote(vote), parentPeer);
        } else {
            //todo I think this goes at some mment later than receiving, and all breaks apart. We need this more resilient (If height >= blockheight we check in the quorum cert for it).
            block_t blk = get_delivered_blk(vote.blk_hash);
            if (blk->self_qc == nullptr)
            {
                //std::cout << "create quorum cert 0" << std::endl;
                blk->self_qc = create_quorum_cert(prop.blk->get_hash());
                blk->self_qc->add_part(config, vote.voter, *vote.cert);
            }
        }
    });
}

void HotStuffBase::do_consensus(const block_t &blk) {
    pmaker->on_consensus(blk);
}

void HotStuffBase::do_decide(Finality &&fin) {
    part_decided++;
    state_machine_execute(fin);
    auto it = decision_waiting.find(fin.cmd_hash);
    if (it != decision_waiting.end())
    {
        it->second(std::move(fin));
        decision_waiting.erase(it);
    }
}

HotStuffBase::~HotStuffBase() {}

void HotStuffBase::start(std::vector<std::tuple<NetAddr, pubkey_bt, uint256_t>> &&replicas, bool ec_loop) {

    const uint8_t fanout = config.fanout;

    uint16_t parent = 0;
    std::set<uint16_t> children;
    auto level = 0;
    auto maxFanout = fanout;
    auto currentChildren = 0;
    uint8_t preLevel = 0;

    for (size_t i = 0; i < replicas.size(); i++)
    {
        const auto remaining = replicas.size() - i;
        const double processesOnLevel = std::ceil(std::pow(fanout, level));

        if (i != 0) {
            currentChildren++;
        }
        if (currentChildren > maxFanout) {
            parent++;
            currentChildren = 1;
        }

        if (fanout < replicas.size()) {
            if (currentChildren == 1 && processesOnLevel > remaining) {
                auto previousProcesses = 0;
                for (auto l = 0; l < level - 1; l++) {
                    previousProcesses += std::ceil(std::pow(fanout, l));
                }
                auto doneParents = parent - previousProcesses;
                auto parents = std::ceil(std::pow(fanout, level - 1));
                auto perParent = std::floor(remaining / (parents - doneParents));
                maxFanout = perParent;
            }
        }

        auto &addr = std::get<0>(replicas[i]);
        auto cert_hash = std::move(std::get<2>(replicas[i]));
        valid_tls_certs.insert(cert_hash);
        salticidae::PeerId peer{cert_hash};
        HotStuffCore::add_replica(i, peer, std::move(std::get<1>(replicas[i])));
        if (addr != listen_addr)
        {
            peers.push_back(peer);
            pn.add_peer(peer);
            pn.set_peer_addr(peer, addr);
        }

        if (id == parent) {
            if (id != i) {
                std::cout << id << " add child: " << i << " level: " << level << "ps: " << processesOnLevel << " "
                          << std::to_string(remaining) << " " << std::to_string(maxFanout) << std::endl;
                childPeers.insert(peer);
                children.insert(i);
            }
        } else if (id == i) {
            std::cout << id << " set parent: " << parent << " " << maxFanout << " " << currentChildren << std::endl;
            parentPeer = peers[parent];
        } else if (i != 0 && children.find(parent) != children.end()) {
            std::cout << id << " add indirect child: " << i << std::endl;
            children.insert(i);
        }

        if (i == static_cast<size_t>(std::pow(fanout, level)) + preLevel) {
            preLevel = static_cast<size_t>(std::pow(fanout, level));
            level++;
        }
    }

    vector<PeerId> newPeers;
    copy(peers.begin(), peers.end(), back_inserter(newPeers));

    std::shuffle(newPeers.begin(), newPeers.end(), std::mt19937(std::random_device()()));
    for (const PeerId& peer : newPeers) {
        if (childPeers.count(peer) > 0 || peer == peers[parent]) {
            pn.conn_peer(peer);
            usleep(1000);
        }
    }

    std::cout << " total children: " << children.size() << std::endl;
    numberOfChildren = children.size();

    /* ((n - 1) + 1 - 1) / 3 */
    uint32_t nfaulty = peers.size() / 3;
    if (nfaulty == 0)
        LOG_WARN("too few replicas in the system to tolerate any failure");
    on_init(nfaulty);
    pmaker->init(this);
    if (ec_loop)
        ec.dispatch();

    cmd_pending.reg_handler(ec, [this](cmd_queue_t &q) {
        std::pair<uint256_t, commit_cb_t> e;
        while (q.try_dequeue(e))
        {
            ReplicaID proposer = pmaker->get_proposer();

            const auto &cmd_hash = e.first;
            auto it = decision_waiting.find(cmd_hash);
            if (it == decision_waiting.end())
                it = decision_waiting.insert(std::make_pair(cmd_hash, e.second)).first;
            else
                e.second(Finality(id, 0, 0, 0, cmd_hash, uint256_t()));
            if (proposer != get_id()) continue;
            cmd_pending_buffer.push(cmd_hash);
            if (cmd_pending_buffer.size() >= blk_size)
            {
                std::vector<uint256_t> cmds;
                for (uint32_t i = 0; i < blk_size; i++)
                {
                    cmds.push_back(cmd_pending_buffer.front());
                    cmd_pending_buffer.pop();
                }
                pmaker->beat().then([this, cmds = std::move(cmds)](ReplicaID proposer) {
                    if (proposer == get_id())
                        on_propose(cmds, pmaker->get_parents());
                });
                return true;
            }
        }
        return false;
    });
}

}
