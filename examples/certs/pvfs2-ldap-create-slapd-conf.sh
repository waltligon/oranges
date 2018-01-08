#!/bin/bash
# pvfs2-ldap-create-slapd-ldif.sh
# Create a new slapd conf file in ldif format, add it to the system, and then
# start the slapd server.

usage ()
{
    echo "USAGE: $0 [-h] [-c] [-i openldap installation directory] [-p pid directory] [-d data dir] [-m mod dir] [-s suffix dn]"
    echo "    -h: help"
    echo "    -c: output ldif server config file only (to ./slapd.ldif)"
    echo "    -i: openldap installation directory; default is /usr" 
    echo "    -p: location where pid file is located when slapd is started"
    echo "    -d: location where database files should be created"
    echo "    -m: location where modules are kept (optional)"
    echo "    -s: suffix dn: base (topmost) dn of tree, default based on hostname, e.g. clemson.edu => 'dc=clemson,dc=edu' "
}

randpw ()
{
    chars="abcdefghijklmnopqrstuvwxyz0123456789"
    for ((i=0; $i < 10; i++))
    do        
        pass+=${chars:$(($RANDOM % 36)):1}
    done
    
    echo $pass
}

while getopts "chi:f:p:d:m:a:s:w:" option
do
    case $option in
        c)
            confonly=yes
        ;;
        f)
            conffile=$OPTARG
        ;;
        p)
            pidfile=$OPTARG
        ;;
        d)
            datadir=$OPTARG
        ;;
        m)
            moddir=$OPTARG
        ;;
        s)
            #lowercase
            suffix=${OPTARG,}
        ;;
        *)
            usage
            exit 1
        ;;
    esac
done

# defaults
if [ ! $conffile ]; then
    conffile=`grep ^configfile= $initscript | cut -d= -f2`
fi
confdir=`dirname $conffile`

if [ ! $pidfile ]; then
    pidfile=`grep ^slapd_pidfile= $initscript | cut -d= -f2`
fi
rundir=`dirname $pidfile`

if [ ! $datadir ]; then
    datadir=/var/lib/ldap
fi

if [ ! $moddir ]; then
    if [ -d /usr/lib64/openldap ]; then
       moddir=/usr/lib64/openldap
    else
       moddir=/usr/lib/openldap
    fi
fi

# sanity check
if [ ! \( -f $initscript -a -d $datadir \) ]; then
    echo "Could not locate LDAP files/directories... exiting"
    exit 1
fi

if [ ! $suffix ]; then
    # generate based on hostname
    hn=`hostname -f 2> /dev/null`
    if [ ! $hn ]; then
         hn=`hostname 2> /dev/null`
         if [ ! $hn ]; then
             echo "Error: could not retrieve hostname... exiting"
             exit 1
         fi
    fi
    # lowercase
    hn=${hn,}
    suffix="dc=${hn/./,dc=}"
fi

#if [ ! $admindn ]; then
#    admindn="cn=admin,$suffix"
#fi

if [ ! $adminpw ]; then
    adminpw="ldappwd"
fi

# locate slappasswd
slappasswd=`which slappasswd 2> /dev/null`
if [ ! $slappasswd ]; then
    slappasswd=/usr/sbin/slappasswd
    if [ ! -f $slappasswd ]; then
        echo "Error: could not locate slappasswd; ensure on PATH... exiting"
        exit 1
    fi
fi

# get encrypted root password
encpwd=`${slappasswd} -s $adminpw`
if [ $? -ne 0 ]; then
    echo "Error: could not get password hash... exiting"
    exit 1
fi

# write the slapd.conf file
echo -n "Writing conf. file... "
sed "s%__CONFDIR__%${confdir}%;s%__RUNDIR__%${rundir}%;s%__DATADIR__%${datadir}%;s%__MODDIR__%${moddir}%;s%__SUFFIX__%${suffix}%" slapd.ldif.in > slapd.ldif

if [ $? -eq 0 ]; then
    echo "ok"
fi

if [ $confonly ]; then
   exit 0
fi

chmod 644 slapd.conf
if [ $? -ne 0 ]; then
   echo "Warning: could not chmod slapd.conf"
fi

# shut down slapd if necessary
ps -e | grep -q slapd &> /dev/null
if [ $? -eq 0 ]; then
    echo "Shutting down slapd"
    $initscript stop
    sleep 2
fi

# copy configuration file
if [ -f $conffile ]; then
    echo "Backing up existing $conffile to ${conffile}.bak"
    cp -p $conffile ${conffile}.bak
fi
cp -p slapd.conf $conffile
if [ $? -ne 0 ]; then
    echo "Error copying to $conffile... exiting"
    exit 1
fi

# back up configuration dir
if [ -d $confdir/slapd.d ]; then
    echo "Renaming $confdir/slapd.d to $confdir/slapd.d.bak"
    mv $confdir/slapd.d $confdir/slapd.d.bak
fi

# start slapd using init script
$initscript start

sleep 2

#containers=(`echo $suffix | sed 's/,/ /g'`)
#for ((i = ${#containers[*]}-1; i >= 0; i--)); do
#    cname=${containers[$i]#*=}
#    ctype=${containers[$i]%=*}

    # echo "${containers[$i]: $ctype $cname"

#    case $ctype in
#        dc) 
#            class="domain"
#        ;;
#        l)
#            class="locality"
#        ;;
#        o)
#            class="organization"
#        ;;
#        ou)
#            class="organizationalUnit"
#        ;;
#        *)
#            echo "Error: cannot create top level container; use only dc, l, o and ou... exiting"
#            exit 1
#        ;;
#    esac  

# create topmost base containers
dname=${suffix#*=}
dname=${dname%%,*}
ldapadd -D "$admindn" -w "$adminpw" -x <<_EOF
dn: $suffix
dc: $dname
objectClass: domain
_EOF
if [ $? -ne 0 ]; then
    echo "Error: could not create ${suffix}... exiting"
    exit 1
fi

# create user and group containers
ldapadd -D "$admindn" -w "$adminpw" -x <<_EOF
dn: ou=Users,$suffix
ou: Users
objectClass: organizationalUnit

dn: ou=Groups,$suffix
ou: Groups
objectClass: organizationalUnit
_EOF
if [ $? -ne 0 ]; then
    echo "Error: could not create dn=Users,${suffix}... exiting"
    exit 1
fi

# create a user to represent root (not LDAP admin) 
# use random password
ldapadd -D "$admindn" -w "$adminpw" -x <<_EOF
dn: cn=root,ou=Users,$suffix
cn: root
uid: root
objectClass: inetOrgPerson
objectClass: posixAccount
displayName: root
sn: root
uidNumber: 0
gidNumber: 0
homeDirectory: /root
userPassword: `randpw`
_EOF
if [ $? -ne 0 ]; then
    echo "Error: could not create cn=root,ou=Users,${suffix}... exiting"
    exit 1
fi

