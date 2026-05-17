#!/bin/bash

THIS_FULLPATH=$(cd $(dirname "${BASH_SOURCE[0]}") && pwd -P)/$(basename "${BASH_SOURCE[0]}")
THIS_FOLDERPATH=$(cd $(dirname "${BASH_SOURCE[0]}") && pwd -P)

APATH=/home/rogical/vmangos2/bin/
WPATH=/home/rogical/vmangos2/bin/
ASRV_BIN=realmd      #This usually doesnt change. TrinityCore: authserver  MaNGOS: realmd  ArcEmu: whocares?
WSRV_BIN_ORG=m2angosd #This usually doesnt change. TrinityCore: worldserver MaNGOS: mangosd ArcEmu: whocares?
WSRV_BIN=m2angosd
WSRV_SCR=m2angosd

echo "run" >gdbcommands
echo "shell echo -e \"\nCRASHLOG BEGIN\n\"" >>gdbcommands
echo "info program" >>gdbcommands
echo "shell echo -e \"\nBACKTRACE\n\"" >>gdbcommands
echo "bt" >>gdbcommands
echo "shell echo -e \"\nBACKTRACE FULL\n\"" >>gdbcommands
echo "bt full" >>gdbcommands
echo "shell echo -e \"\nTHREADS\n\"" >>gdbcommands
echo "info threads" >>gdbcommands
echo "shell echo -e \"\nTHREADS BACKTRACE\n\"" >>gdbcommands
echo "thread apply all bt full" >>gdbcommands

DEBUG=true

#WORLD FUNCTIONS
startWorld() {
	if [ "$(screen -ls | grep $WSRV_SCR)" ]; then
		echo $WSRV_BIN is already running
	else
		cd $WPATH
		screen -AmdS "$WSRV_SCR" sh -c "ulimit -c unlimited && taskset -c 0-15 $THIS_FULLPATH $WSRV_BIN $DEBUG"
                echo "$WSRV_BIN 已在 P-Core (0-15) 启动。"

		#screen -AmdS $WSRV_SCR $THIS_FULLPATH $WSRV_BIN $DEBUG
		#echo $WSRV_BIN is alive
	fi
}

restartWorld() {
	screen -S $WSRV_SCR -X stuff "saveall$(printf \\r)"
	echo saved all characters, and server restart initialized
	screen -S $WSRV_SCR -X stuff "server restart 5$(printf \\r)"
}

restartWorld5() {
	screen -S $WSRV_SCR -X stuff "saveall$(printf \\r)"
	echo saved all characters, and server restart initialized
	screen -S $WSRV_SCR -X stuff "server restart 300$(printf \\r)"
}

stopWorld() {
	# 1. Graceful shutdown: tell mangosd to save all players, broadcast a
	#    countdown, and exit cleanly. This is the safest path (no data loss).
	screen -S $WSRV_SCR -X stuff "server shutdown 5$(printf \\r)" 2>/dev/null
	echo "Graceful shutdown initiated (5s grace period for players)..."
	sleep 6

	# 2. Wait for mangosd to exit on its own. Poll every second, bail out as
	#    soon as the binary is gone. 60s ceiling covers heavy player loads.
	local waited=0
	while [ $waited -lt 60 ]; do
		if ! pgrep -x "$WSRV_BIN" > /dev/null; then
			echo "$WSRV_BIN exited cleanly after ${waited}s."
			break
		fi
		sleep 1
		waited=$((waited + 1))
	done

	# 3. Tear down the whole wrapper chain. The wstart loop's bash can be
	#    orphaned and adopted by init when screen dies, leaving a runaway
	#    `killall -9 $WSRV_BIN` loop that would kill any future mangosd
	#    instance the user starts. Kill the wrapper bash first, then any
	#    residual gdb/mangosd, then the screen window.
	pkill -TERM -f "wowadmin.sh $WSRV_BIN" 2>/dev/null
	sleep 1
	pkill -KILL -f "wowadmin.sh $WSRV_BIN" 2>/dev/null
	pkill -KILL -f "gdb .*$WSRV_BIN" 2>/dev/null
	pkill -KILL -x "$WSRV_BIN" 2>/dev/null
	screen -S $WSRV_SCR -X quit &>/dev/null

	echo "$WSRV_BIN and all wrappers are dead."
}

monitorWorld() {
	echo press ctrl+a+d to detach from the server without shutting it down
	sleep 5
	screen -r $WSRV_SCR
}

startbattlebot1() {
	screen -S $WSRV_SCR -X stuff "battlebot autojoin1 on$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "announce 奥山训练营已开启！$(printf \\r)"
	echo 奥山训练营已开启
}
startbattlebot2() {
	screen -S $WSRV_SCR -X stuff "battlebot autojoin2 on$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "announce 战歌训练营已开启！$(printf \\r)"
	echo 战歌训练营已开启
}
startbattlebot3() {
	screen -S $WSRV_SCR -X stuff "battlebot autojoin3 on$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "announce 阿拉希训练营已开启！$(printf \\r)"
	echo 阿拉希训练营已开启
}

stopbattlebot() {
	screen -S $WSRV_SCR -X stuff "battlebot autojoin1 off$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "battlebot autojoin2 off$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "battlebot autojoin3 off$(printf \\r)"
	screen -S $WSRV_SCR -X stuff "announce 战场训练营已关闭。$(printf \\r)"
	echo 战场训练营已关闭
}

#AUTH FUNCTIONS
startAuth() {
	if [ "$(screen -ls | grep $ASRV_BIN)" ]; then
		echo $ASRV_BIN is already running
	else
		cd $APATH
		screen -AmdS $ASRV_BIN $THIS_FULLPATH $ASRV_BIN
		echo $ASRV_BIN is alive
	fi
}

stopAuth() {
	screen -S $ASRV_BIN -X kill &>/dev/null
	echo $ASRV_BIN is dead
}

restartAuth() {
	stopAuth
	startAuth
	echo $ASRV_BIN restarted
}

monitorAuth() {
	echo press ctrl+a+d to detach from the server without shutting it down
	sleep 5
	screen -r $ASRV_BIN
}

#FUNCTION SELECTION
case "$1" in
$WSRV_BIN)
	if [ "$2" == "true" ]; then
		while x=1; do
			gdb $WPATH/$WSRV_BIN --batch -x gdbcommands | tee current
			NOW=$(date +"%s-%d-%m-%Y")
			mkdir -p $THIS_FOLDERPATH/crashes
			mv current $THIS_FOLDERPATH/crashes/$NOW.log &>/dev/null
			killall -9 $WSRV_BIN
			echo $NOW $WSRV_BIN stopped, restarting! | tee -a $THIS_FULLPATH.log
			echo crashlog available at: $THIS_FOLDERPATH/crashes/$NOW.log
			sleep 1
		done
	else
		while x=1; do
			./$WSRV_BIN
			NOW=$(date +"%s-%d-%m-%Y")
			echo $NOW $WSRV_BIN stopped, restarting! | tee -a $THIS_FULLPATH.log
			sleep 1
		done
	fi
	;;
$ASRV_BIN)
	while x=1; do
		./$ASRV_BIN
		NOW=$(date +"%s-%d-%m-%Y")
		echo $NOW $ASRV_BIN stopped, restarting! | tee -a $THIS_FULLPATH.log
		sleep 1
	done
	;;
"wstart")
	startWorld
	;;
"wdstart")
	DEBUG=true
	startWorld
	;;
"wrestart")
	restartWorld
	;;
"wrestart5")
	restartWorld5
	;;
"wstop")
	stopWorld
	;;
"wmonitor")
	monitorWorld
	;;

"astart")
	startAuth
	;;
"arestart")
	restartAuth
	;;
"astop")
	stopAuth
	;;
"amonitor")
	monitorAuth
	;;

"start")
	startWorld
	startAuth
	;;
"stop")
	stopWorld
	stopAuth
	;;
"restart")
	restartWorld
	restartAuth
	;;
"startbattlebot1")
	startbattlebot1
	;;
"startbattlebot2")
	startbattlebot2
	;;
"startbattlebot3")
	startbattlebot3
	;;
"stopbattlebot")
	stopbattlebot
	;;
*)
	echo Your argument is invalid
	echo "usage: start | stop | restart | wstart | wdstart | wrestart | wstop | wmonitor | astart | arestart | astop | amonitor"
	exit 1
	;;
esac

