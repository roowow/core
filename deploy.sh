cd /home/rogical/vmangos-dev/core

git pull

cd ../build

make -j20

make install

cd ..
./wowadmin.sh wrestart

sleep 20

./wowadmin.sh startbattlebot1
./wowadmin.sh startbattlebot2
./wowadmin.sh startbattlebot3
