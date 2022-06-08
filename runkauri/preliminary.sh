sudo apt-get update
#sudo apt-get install docker.io libssl-dev libuv1-dev cmake make software-properties-common ansible g++ autoconf libtool
#sudo add-apt-repository --yes --update ppa:ansible/ansible
sudo apt-get -y install docker.io
#Kauri
git clone https://github.com/Raycoms/Kauri-Public.git
cd Kauri-Public/runkauri
sudo docker build -t kauri .
"sudo apt-get update;sudo apt-get -y install docker.io; git clone https://github.com/Raycoms/Kauri-Public.git;cd Kauri-Public/runkauri;sudo docker build -t kauri ."
sudo docker swarm init
sudo docker network create --driver=overlay --subnet=10.1.0.0/16 kauri_network
#cd ..
#hotstuff
#git clone https://github.com/hot-stuff/libhotstuff.git
#cd libhotstuff/
#git submodule update --init --recursive
#sudo apt install software-properties-common
#sudo apt install ansible
#sudo apt-get install g++
#sudo apt-get install autoconf libtool
#cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON -DHOTSTUFF_PROTO_LOG=ON
#make
