FORCE=0
for arg in "$@"; do
    [ "$arg" = "--force" ] && FORCE=1
done

cd /home/rogical/vmangos-dev/core

BEFORE=$(git rev-parse HEAD)
git pull
AFTER=$(git rev-parse HEAD)

if [ "$BEFORE" = "$AFTER" ] && [ "$FORCE" = "0" ]; then
    echo "No new commits, skipping deploy."
    exit 0
fi

if [ "$BEFORE" = "$AFTER" ]; then
    echo "No new commits, but --force specified, deploying..."
else
    echo "New commits detected ($BEFORE -> $AFTER), deploying..."
fi

cd ../build

make -j20

make install

cd ..
BUILD_TIME=$(date '+%Y-%m-%d %H:%M')
sed -i "s#^Motd = .*#Motd = \"欢迎进入开发测试服！ | 版本：$BUILD_TIME\"#" /home/rogical/vmangos-dev/etc/mangosd.conf

./wowadmin.sh wrestart

sleep 20

./wowadmin.sh startbattlebot1
./wowadmin.sh startbattlebot2
./wowadmin.sh startbattlebot3
