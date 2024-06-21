#!/bin/bash

trap "docker stack rm kauriservice" EXIT

FILENAME=kauri.yaml
EXPORT_FILENAME=kauri-temp.yaml

ORIGINAL_STRING=thecmd
QTY1_STRING=theqty1
QTY2_STRING=theqty2

FILENAME2="experiments"
LINES=$(cat $FILENAME2 | grep "^[^#;]")
# Each LINE in the experiment file is one experimental setup
for LINE in $LINES
do

  echo '---------------------------------------------------------------'
  echo $LINE
  IFS=':' read -ra split <<< "$LINE"

  sed  "s/${ORIGINAL_STRING}/${split[0]}/g" $FILENAME > $EXPORT_FILENAME
  sed  -i "s/${QTY1_STRING}/${split[1]}/g" $EXPORT_FILENAME
  sed  -i "s/${QTY2_STRING}/${split[2]}/g" $EXPORT_FILENAME

  echo '**********************************************'
  echo "*** This setup needs ${split[3]} physical machines! ***"
  echo '**********************************************'

  # baseline without failing nodes
  # Deploy experiment
  docker stack deploy -c kauri-temp.yaml kauriservice &
  # Docker startup time + 5*60s of experiment runtime
  sleep 450

  echo "******************** Baseline results ******************** "
  # Collect and print results.
  for container in $(docker ps -q -f name="server")
  do
          if [ ! $(docker exec -it $container bash -c "cd Kauri-Public && test -e log0") ]
          then

            docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'commit <block'"
            docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'x now state'"
            docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'Average'"
          fi
  done
  echo "******************** End of Baseline results ******************** "

  docker stack rm kauriservice
  sleep 30

  for i in {1..2}
  do
	  break
        # Deploy experiment
        docker stack deploy -c kauri-temp.yaml kauriservice &
        # Docker startup time + 5*60s of experiment runtime
        sleep 150
        for container in $(docker ps -q -f name="server1")
        do
          docker stop $container
                break
        done

        sleep 300

        echo "******************** Failing node results ******************** "
        # Collect and print results.
        for container in $(docker ps -q -f name="server")
        do
                if [ ! $(docker exec -it $container bash -c "cd Kauri-Public && test -e log0") ]
                then
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'commit <block'"
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'x now state'"
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'Average'"
	  	            #docker exec -it $container bash -c "cd Kauri-Public && tac log*"
		              #docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep 'hotstuff proto'"
                  #docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep 'hotstuff info'"
		              echo "this log is from container:${container}"
                fi
        done

        for container in $(docker ps -q -f name="server1")
        do
                if [ ! $(docker exec -it $container bash -c "cd Kauri-Public && test -e log0") ]
                then
                        n=$((n +1))
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'commit <block'"
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'x now state'"
                  docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep -m1 'Average'"
                  #docker exec -it $container bash -c "cd Kauri-Public && tac log*"
                  #docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep 'hotstuff proto'"
                  #docker exec -it $container bash -c "cd Kauri-Public && tac log* | grep 'hotstuff info'"

		              echo "this log is from container:${container}"
                fi
        done
        echo "******************** End of Failing node results ******************** "

	      #sleep 3000
        docker stack rm kauriservice
        sleep 30

  done
done