#! /bin/sh
##
## ovs.sh --
##
##   Help script for xcluster ovl/ovs.
##
## Commands;
##

prg=$(basename $0)
dir=$(dirname $0); dir=$(readlink -f $dir)
me=$dir/$prg
tmp=/tmp/${prg}_$$

die() {
    echo "ERROR: $*" >&2
    rm -rf $tmp
    exit 1
}
help() {
    grep '^##' $0 | cut -c3-
    rm -rf $tmp
    exit 0
}
test -n "$1" || help
echo "$1" | grep -qi "^help\|-h" && help

log() {
	echo "$prg: $*" >&2
}
dbg() {
	test -n "$__verbose" && echo "$prg: $*" >&2
}

##   env
##     Print environment.
##
cmd_env() {

	test -n "$SYSD" || SYSD=$XCLUSTER_WORKSPACE/sys
	if test "$cmd" = "env"; then
		set | grep -E '^(__.*|SYSD)='
		return 0
	fi

	test -n "$XCLUSTER" || die 'Not set [$XCLUSTER]'
	test -x "$XCLUSTER" || die "Not executable [$XCLUSTER]"
	eval $($XCLUSTER env)
}

##   build [--dest=$GOPATH/src/github.com/openvswitch/ovs]
##     Clone/pull and build OVS.
cmd_build() {
	cmd_env
	test -n "$__dest" || __dest=$GOPATH/src/github.com/openvswitch/ovs
	if test -d $__dest; then
		cd $__dest
		git pull
	else
		local pdir=$(basename $__dest)
		mkdir -p $pdir || die "mkdir $pdir"
		cd $pdir
		git clone https://github.com/openvswitch/ovs.git || die "git clone"
		cd $__dest
	fi

	local bpflibd=$(readlink -f $__kobj/source)/tools/lib/bpf/build/usr
	./boot.sh
	if test -d $bpflibd; then
		LDFLAGS=-L$bpflibd/lib64 CPPFLAGS=-I$bpflibd/include \
			./configure --enable-afxdp
	else
		./configure
	fi
	make -j$(nproc) || die make
	make DESTDIR=$SYSD install || die "make install"
	if test -d $bpflibd; then
		log "Building with XDP support"
	else
		log "Building WITHOUT XDP support"
	fi
}

##   man [command]
##     Show a ovs man-page. List if no command is specified.
cmd_man() {
	cmd_env
	MANPATH=$SYSD/usr/local/share/man
	if test -z "$1"; then
		local f
		mkdir -p $tmp
		for f in $(find $MANPATH -type f); do
			basename $f >> $tmp/man
		done
		cat $tmp/man | sort | column
		return 0
	fi
	export MANPATH
	xterm -bg '#ddd' -fg '#222' -geometry 80x45 -T $1 -e man $1 &
}

##
##   test --list
##   test [--xterm] [--no-stop] [test...] > logfile
##     Exec tests
##
cmd_test() {
	if test "$__list" = "yes"; then
        grep '^test_' $me | cut -d'(' -f1 | sed -e 's,test_,,'
        return 0
    fi

	cmd_env
    start=starts
    test "$__xterm" = "yes" && start=start
    rm -f $XCLUSTER_TMP/cdrom.iso

    if test -n "$1"; then
        for t in $@; do
            test_$t
        done
    else
		test_L2
    fi      

    now=$(date +%s)
    tlog "Xcluster test ended. Total time $((now-begin)) sec"

}
##   test start - Start cluster
test_start() {
	export __image=$XCLUSTER_HOME/hd.img
	echo "$XOVLS" | grep -q private-reg && unset XOVLS
	test -n "$__nrouters" || export __nrouters=0
	test -n "$TOPOLOGY" && \
		. $($XCLUSTER ovld network-topology)/$TOPOLOGY/Envsettings
	xcluster_start network-topology iptools netns ovs
	otc 1 version
}
##   test L2 (default) - Setup an L2 network and test with ping
test_L2() {
	tlog "=== ovs: L2 network"
	test_start
	otcw create_bridge
	otcw create_netns
	otcw add_ports
	otc 1 ping_all
	xcluster_stop
}

##   test basic_flow - Setup OpenFlow between 2 PODs on vm-002
test_basic_flow() {
	tlog "=== ovs: Basic OpenFlow"
	export xcluster_PODIF=eth0
	test_start
	otcw create_ofbridge
	otcw create_netns
	otcw create_veth
	otcw attach_veth
	otc 2 "ping_negative --pod=vm-002-ns02 172.16.2.1"
	xcluster_stop
}

##
. $($XCLUSTER ovld test)/default/usr/lib/xctest
indent=''

# Get the command
cmd=$1
shift
grep -q "^cmd_$cmd()" $0 $hook || die "Invalid command [$cmd]"

while echo "$1" | grep -q '^--'; do
    if echo $1 | grep -q =; then
	o=$(echo "$1" | cut -d= -f1 | sed -e 's,-,_,g')
	v=$(echo "$1" | cut -d= -f2-)
	eval "$o=\"$v\""
    else
	o=$(echo "$1" | sed -e 's,-,_,g')
	eval "$o=yes"
    fi
    shift
done
unset o v
long_opts=`set | grep '^__' | cut -d= -f1`

# Execute command
trap "die Interrupted" INT TERM
cmd_$cmd "$@"
status=$?
rm -rf $tmp
exit $status
