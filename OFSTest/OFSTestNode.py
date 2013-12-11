#!/usr/bin/python

###############################################################################
#
# OFSTestNode
# 
# This is the base class for all test nodes that run OrangeFS. Local and 
# remote; Client, Server, and Build nodes. 
#
# OFSTestNode is an abstract class. All nodes should be of a more specific 
# subclass type.
#
################################################################################

import os
import subprocess
import shlex
import cmd
import time
import sys
import xml.etree.ElementTree as ET
import traceback
#import scriptine.shell

###############################################################################################'
#
# class OFSTestNode(object)
#
#
# This class represents all machines that are part of the OFS process. This includes local, remote
# and remote-ec2 based machines.
#
# This program assumes that the OFSTestNode is a *nix machine operating a bash shell. 
# MacOSX functionality may be limited. Windows local nodes are not currently supported.
#
# The methods are broken down into the following:
#
# Object functions: Gets and sets for the object data.
# Utility functions: Basic shell functionality.
# OFSTestServer functions: Configure and run OrangeFS server
# OFSTestBuilder functions: Compile OrangeFS and build rpms/dpkgs.
# OFSTestClient functions: Configure and run OrangeFS client
#
################################################################################################

# global variable for batch counting
batch_count = 0

class OFSTestNode(object):
    
      # initialize node. We don't have much info for the base class.
    def __init__(self):
        
        #------
        #
        # identity variables
        #
        #-------
        
        # list of OrangeFS Aliases
        self.alias_list = None
        
        # ip address on internal network
        self.ip_address = ""
        
        # ip address on external network
        self.ext_ip_address = self.ip_address
        
        # current hostname
        self.host_name = ""
        
        # operating system
        self.distro = ""
        
        # package system (rpm/dpkg)
        self.package_system=""
        
        # location of the linux kernel source
        self.kernel_source_location = ""
        
        # kernel version (uname -r)
        self.kernel_version=""
        
        # is this a remote machine?
        self.is_remote=True
        
        # is this an ec2/openstack instance?
        self.is_ec2=False
        
        # type of processor (i386/x86_64)
        self.processor_type = "x86_64"
        
        #------
        #
        # shell variables
        #
        #-------
        
        # main user login. Usually ec2-user for ec2 instances.
        self.current_user = ""
        
        # current working directory
        self.current_directory = "~"
        #previous directory
        self.previous_directory = "~"
        
        # current environment variables
        self.current_environment = {}
        
        # commands written to a batch file
        self.batch_commands = []
        
        #-------------------------------------------------------
        # sshKeys
        #
        #
        # The node key file is the file on the node that contains the key to access this machine.
        # The local key file is the location of the key on the local host. Local key file is ALWAYS on the
        # localhost.
        #
        # The keytable is a dictionary of locations of keys to remote machines on the current node.
        #
        #--------------------------------------------------------

        self.sshLocalKeyFile = ""
        self.sshNodeKeyFile = ""
        self.keytable = {}
        
        #----------------------------------------------------------
        #
        # orangefs related variables
        #
        #----------------------------------------------------------
       
        # This is the location of the OrangeFS source
        self.ofs_source_location = ""
        
        # This is the location of the OrangeFS storage
        self.ofs_storage_location = ""

        # This is where OrangeFS is installed. Defaults to /opt/orangefs
        self.ofs_installation_location = ""
        
        # This is the location of the third party benchmarks
        self.ofs_extra_tests_location = ""

        # This is the mount_point for OrangeFS
        self.ofs_mount_point = ""
        
        # This is the OrangeFS service name. pvfs2-fs < 2.9, orangefs >= 2.9
        self.ofs_fs_name="orangefs"
        
        # svn branch (or ofs source directory name)
        self.ofs_branch = ""
        
        # default tcp port
        self.ofs_tcp_port = "3396"
        
        # berkeley db4 location
        self.db4_dir = "/opt/db4"
        self.db4_lib_dir = self.db4_dir+"/lib"
        self.ofs_conf_file = None
        
        self.mpich2_installation_location = ""
        self.mpich2_source_location = ""
        
        self.openmpi_installation_location = ""
        self.openmpi_source_location = ""
        self.openmpi_version = ""  

    #==========================================================================
    # 
    # currentNodeInformation
    #
    # Logs into the node to gain information about the system
    #
    #==========================================================================
    

    def currentNodeInformation(self):
        
        self.distro = ""
        #print "Getting current node information"
        
        # can we ssh in? We'll need the group if we can't, so let's try this first.
        self.current_group = self.runSingleCommandBacktick(command="ls -l /home/ | grep %s | awk {'print \\$4'}" % self.current_user)
        
        print "Current group is "+self.current_group

        # Direct access as root not good. Need to get the actual user in
        # Gross hackery for SuseStudio images. OpenStack injects key into root, not user.
                    
        if self.current_group.rstrip() == "":
            self.current_group = self.runSingleCommandBacktick(command="ls -l /home/ | grep %s | awk {'print \\$4'}" % self.current_user,remote_user="root")
            print "Current group (from root) is "+self.current_group
            if self.current_group.rstrip() == "":
                print "Could not access node at "+self.ext_ip_address+" via ssh"
                exit(-1)
            
            
            # copy the ssh key to the user's directory
            rc = self.runSingleCommand(command="cp -r /root/.ssh /home/%s/" % self.current_user,remote_user="root")
            if rc != 0:
                print "Could not copy ssh key from /root/.ssh to /home/%s/ " % self.current_user
                exit(rc)
            
            #get the user and group name of the home directory
            
            # change the owner of the .ssh directory from root to the login user
            rc = self.runSingleCommand(command="chown -R %s:%s /home/%s/.ssh/" % (self.current_user,self.current_group,self.current_user),remote_user="root") 
            if rc != 0:
                print "Could not change ownership of /home/%s/.ssh to %s:%s" % (self.current_user,self.current_user,self.current_group)
                exit(rc)
            
        # get the hostname
        self.host_name = self.runSingleCommandBacktick("hostname")
        
        # Torque doesn't like long hostnames. Truncate the hostname to 15 characters if necessary.
        if len(self.host_name) > 15:
            short_host_name = self.host_name[:15]
            self.runSingleCommandAsBatch("sudo bash -c 'echo %s > /etc/hostname'" % short_host_name)
            self.runSingleCommandAsBatch("sudo hostname %s" % short_host_name)
            print "Truncating hostname %s to %s" % (self.host_name,short_host_name)
            self.host_name = self.host_name[:15]

        # get kernel version and processor type
        self.kernel_version = self.runSingleCommandBacktick("uname -r")
        self.processor_type = self.runSingleCommandBacktick("uname -p")
        
        
        # Find the distribution. Unfortunately Linux distributions each have their own file for distribution information.
            
        # information for ubuntu and suse is in /etc/os-release

        if self.runSingleCommandBacktick('find /etc -name "os-release" 2> /dev/null').rstrip() == "/etc/os-release":
            #print "SuSE or Ubuntu based machine found"
            pretty_name = self.runSingleCommandBacktick("cat /etc/os-release | grep PRETTY_NAME")
            [var,self.distro] = pretty_name.split("=")
        # for redhat based distributions, information is in /etc/system-release
        elif self.runSingleCommandBacktick('find /etc -name "redhat-release" 2> /dev/null').rstrip() == "/etc/redhat-release":
            #print "RedHat based machine found"
            self.distro = self.runSingleCommandBacktick("cat /etc/redhat-release")
        elif self.runSingleCommandBacktick('find /etc -name "lsb-release" 2> /dev/null').rstrip() == "/etc/lsb-release":
            #print "Ubuntu based machine found"
            #print self.runSingleCommandBacktick("cat /etc/lsb-release ")
            pretty_name = self.runSingleCommandBacktick("cat /etc/lsb-release | grep DISTRIB_DESCRIPTION")
            #print "Pretty name " + pretty_name
            [var,self.distro] = pretty_name.split("=")    
        # Mac OS X 
        elif self.runSingleCommandBacktick('find /etc -name "SuSE-release" 2> /dev/null').rstrip() == "/etc/SuSE-release":
            self.distro = self.runSingleCommandBacktick("head -n 1 /etc/SuSE-release").rstrip()
        elif self.runSingleCommandBacktick("uname").rstrip() == "Darwin":
            #print "Mac OS X based machine found"
            self.distro = "Mac OS X-%s" % self.runSingleCommandBacktick("sw_vers -productVersion")
        
        
        # print out node information
        print "Node: %s %s %s %s" % (self.host_name,self.distro,self.kernel_version,self.processor_type)
        
        
       
      #==========================================================================
      # 
      # Utility functions
      #
      # These functions implement basic shell functionality 
      #
      #==========================================================================


      # Change the current directory on the node to run scripts.
    def changeDirectory(self, directory):
        self.current_directory = directory
      
      # set an environment variable to a value
    def setEnvironmentVariable(self,variable,value):
        self.current_environment[variable] = value
      
      # The setenv parameter should be a string 
    def setEnvironment(self, setenv):
        #for each line in setenv
        variable_list = setenv.split('\n')
        for variable in variable_list:
          #split based on the equals sign
          vname,value = variable.split('=')
          self.setEnvironmentVariable(vname,value)

      # Clear all environment variables
    def clearEnvironment(self):
          self.current_environment = {}
      
      # return current directory
    def printWorkingDirectory(self):
        return self.current_directory
     
      # Add a command to the list of commands being run.
      # This is a single line of a shell script.
    def addBatchCommand(self,command):
        self.batch_commands.append(command)
    
    def runSingleCommand(self,command,output=[],remote_user=None):
        
        # This runs a single command and returns the return code of that command
        # command, stdout, and stderr are in the output list
        
        #print command
        if remote_user==None:
            remote_user = self.current_user
        
        command_line = self.prepareCommandLine(command=command,remote_user=remote_user)
        
        #if "awk" in command_line:
        #    print command_line
        #print command_line
        p = subprocess.Popen(command_line,shell=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=-1)
        
        # clear the output list, then append stdout,stderr to list to get pass-by-reference to work
        del output[:]
        output.append(command_line)
        for i in p.communicate():
            output.append(i)

        return p.returncode
        

      
    def runSingleCommandBacktick(self,command,output=[],remote_user=None):
        # This runs a single command and returns the stdout of that command.
        if remote_user==None:
            remote_user = self.current_user
      
        
        self.runSingleCommand(command=command,output=output,remote_user=remote_user)
        if len(output) >= 2:
            return output[1].rstrip('\n')
        else:
            return ""
    
    def runOFSTest(self,package,test_function,output=[],logfile="",errfile=""):
        # This method runs an OrangeFS test on the given node
        #
        # Output and errors are written to the output and errfiles
        # 
        # return is return code from the test function
        #
       
        print "Running test %s-%s" % (package,test_function.__name__)
        
        if logfile == "":
            logfile = "%s-%s.log" % (package,test_function.__name__)
        
                
        # Run the test function
        rc = test_function(self,output)

        try:
            # write the command, return code, stdout and stderr of last program to logfile
            logfile_h = open(logfile,"w+")
            logfile_h.write('COMMAND:' + output[0]+'\n')
            logfile_h.write('RC: %r\n' % rc)
            logfile_h.write('STDOUT:' + output[1]+'\n')
            logfile_h.write('STDERR:' + output[2]+'\n')
            
        except:
            
            traceback.print_exc()
            rc = -99
        
        logfile_h.close()
            
        
        return rc
        
        
    
    def prepareCommandLine(self,command,outfile="",append_out=False,errfile="",append_err=False,remote_user=None):
        # Implimented in the client. Should not be here.
        print "This should be implimented in the subclass, not in OFSTestNode."
        print "Trying naive attempt to create command list."
        return command
       
      # Run a single command as a batchfile. Some systems require this for passwordless sudo
    def runSingleCommandAsBatch(self,command,output=[]):
        self.addBatchCommand(command)
        self.runAllBatchCommands(output)
    
    def runBatchFile(self,filename,output=[]):
        #copy the old batch file to the batch commands list
        batch_file = open(filename,'r')
        self.batch_commands = batch_file.readlines()
        
        # Then run it
        self.runAllBatchCommands(output)

    # copy files from the current node to a destination node.
    def copyToRemoteNode(self,source, destinationNode, destination):
        # implimented in subclass
        pass
      
    # copy files from a remote node to the current node.
    def copyFromRemoteNode(self,sourceNode, source, destination):
        # implimented in subclass
        pass
      
    def writeToOutputFile(self,command_line,cmd_out,cmd_err):
        
        outfile = open("output.out","a+")
        outfile.write("bash$ "+command_line)
        outfile.write("\n")
        outfile.write("Output: "+cmd_out)
        outfile.write("Stderr: "+cmd_err)
        outfile.write("\n")
        outfile.write("\n")
        outfile.close()
      
    #---------------------
    #
    # ssh utility functions
    #
    #---------------------
    def getRemoteKeyFile(self,address):
        #print "Looking for %s in keytable for %s" % (address,self.host_name)
        #print self.keytable
        return self.keytable[address]
      
    def addRemoteKey(self,address,keylocation):
        #
        #This method adds the location of the key for machine at address to the keytable.
        #
        self.keytable[address] = keylocation
     
     
    def copyLocal(self, source, destination, recursive):
        # This runs the copy command locally 
        rflag = ""
        # verify source file exists
        if recursive == True:
            rflag = "-a"
        else:
            rflag = ""
          
        rsync_command = "rsync %s %s %s" % (rflag,source,destination)
        output = []
        rc = self.runSingleCommand(rsync_command, output)
        if rc != 0:
            print rsync_command+" failed!"
            print output
        return rc
      
 
    #============================================================================
    #
    # OFSBuilderFunctions
    #
    # These functions implement functionality to build OrangeFS
    #
    #=============================================================================
    
    #-------------------------------
    #
    # updateNode
    #
    # This function updates the software on the node via the package management system
    #
    #-------------------------------
    
    
    def updateNode(self):
        #print "Distro is " + self.distro
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            self.addBatchCommand("sudo DEBIAN_FRONTEND=noninteractive apt-get -y update")
            self.addBatchCommand("sudo DEBIAN_FRONTEND=noninteractive apt-get -y dist-upgrade < /dev/zero")
        elif "suse" in self.distro.lower():
            self.addBatchCommand("sudo zypper --non-interactive update")
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
            self.addBatchCommand("sudo yum update --disableexcludes=main -y")
            # Uninstall the old kernel
            self.addBatchCommand("sudo rpm -e kernel-`uname -r`")
            #Update grub from current kernel to installed kernel
            self.addBatchCommand('sudo perl -e "s/`uname -r`/`rpm -q --queryformat \'%{VERSION}-%{RELEASE}.%{ARCH}\n\' kernel`/g" -p -i /boot/grub/grub.conf')
        
        self.runAllBatchCommands()
        print "Node "+self.host_name+" at "+self.ip_address+" updated. Rebooting."
        self.runSingleCommandAsBatch("sudo /sbin/reboot")
    
    #-------------------------------
    #
    # installTorqueServer
    #
    # This function installs and configures the torque server from the package management system on the node.
    #
    #-------------------------------
    def installTorqueServer(self):
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            batch_commands = '''

                #install torque
                echo "Installing TORQUE from apt-get"
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q torque-server torque-scheduler torque-client torque-mom < /dev/null 
                sudo bash -c "echo %s > /etc/torque/server_name"
                sudo bash -c "echo %s > /var/spool/torque/server_name"
            ''' % (self.host_name,self.host_name)
            self.addBatchCommand(batch_commands)

        elif "suse" in self.distro.lower():
            
            
            print "TODO: Torque for "+self.distro
            return

            batch_commands = '''
            

            echo "Installing TORQUE from devorange: "
            echo "wget -r -np -nd http://devorange.clemson.edu/pvfs/${SYSTEM}/RPMS/${ARCH}/"
            wget -r -np -nd http://devorange.clemson.edu/pvfs/${SYSTEM}/RPMS/${ARCH}/
            #cd  devorange.clemson.edu/pvfs/openSUSE-12.2/RPMS/x86_64
            ls *.rpm
            sudo rpm -e libtorque2
            sudo rpm -ivh *.rpm
            cd -
            '''
            self.addBatchCommand(batch_commands)
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
            
            if "6." in self.distro:
                batch_commands = '''
                   

                    echo "Adding epel repository"
                    wget http://dl.fedoraproject.org/pub/epel/6/%s/epel-release-6-8.noarch.rpm
                    sudo rpm -Uvh epel-release-6*.noarch.rpm
                    echo "Installing TORQUE from rpm: "
                    sudo yum -y update
                    sudo yum -y install torque-server torque-client torque-mom torque-scheduler munge
                    sudo bash -c '[ -f /etc/munge/munge.key ] || /usr/sbin/create-munge-key'
                    sudo bash -c "echo %s > /etc/torque/server_name"
                    sudo bash -c "echo %s > /var/lib/torque/server_name"

                ''' % (self.processor_type,self.host_name,self.host_name)
            elif "5." in self.distro:
                batch_commands = '''
                   

                    echo "Adding epel repository"
                    wget http://dl.fedoraproject.org/pub/epel/5/%s/epel-release-5-4.noarch.rpm
                    sudo rpm -Uvh epel-release-5*.noarch.rpm
                    echo "Installing TORQUE from rpm: "
                    sudo yum -y update
                    sudo yum -y install torque-server torque-client torque-mom torque-scheduler munge
                    sudo bash -c '[ -f /etc/munge/munge.key ] || /usr/sbin/create-munge-key'
                    sudo bash -c "echo %s > /etc/torque/server_name"
                    sudo bash -c "echo %s > /var/lib/torque/server_name"

                ''' % (self.processor_type,self.host_name,self.host_name)
            else:
                print "TODO: Torque for "+self.distro
                batch_commands = ""
            #print batch_commands    
            self.addBatchCommand(batch_commands)
        
        self.runAllBatchCommands()
     
    def installTorqueClient(self,pbsserver):
        pbsserver_name = pbsserver.host_name
        print "Installing Torque Client for "+self.distro.lower()
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            batch_commands = '''

                #install torque
                echo "Installing TORQUE from apt-get"
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q torque-client torque-mom < /dev/null 
                sudo bash -c 'echo \$pbsserver %s > /var/spool/torque/mom_priv/config' 
                sudo bash -c 'echo \$logevent 255 >> /var/spool/torque/mom_priv/config' 
            ''' % pbsserver_name

            self.addBatchCommand(batch_commands)

        elif "suse" in self.distro.lower():
            # this needs to be fixed
            print "TODO: Torque for "+self.distro
            return

            batch_commands = '''
            

            echo "Installing TORQUE from devorange: "
            echo "wget -r -np -nd http://devorange.clemson.edu/pvfs/${SYSTEM}/RPMS/${ARCH}/"
            wget -r -np -nd http://devorange.clemson.edu/pvfs/${SYSTEM}/RPMS/${ARCH}/
            #cd  devorange.clemson.edu/pvfs/openSUSE-12.2/RPMS/x86_64
            ls *.rpm
            sudo rpm -e libtorque2
            sudo rpm -ivh *.rpm
            cd -
            sudo bash -c 'echo $pbsserver %s > /var/spool/torque/mom_priv/config'
            sudo bash -c 'echo $logevent 255 >> /var/spool/torque/mom_priv/config' 
            ''' % pbsserver_name
            self.addBatchCommand(batch_commands)
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
            
            if "6." in self.distro:
                batch_commands = '''
               
                echo "Adding epel repository"
                wget http://dl.fedoraproject.org/pub/epel/6/%s/epel-release-6-8.noarch.rpm
                sudo rpm -Uvh epel-release-6*.noarch.rpm
                echo "Installing TORQUE from rpm: "
                sudo yum -y update
                sudo yum -y install torque-client torque-mom munge
                sudo bash -c '[ -f /etc/munge/munge.key ] || /usr/sbin/create-munge-key'
                sudo bash -c 'echo \$pbsserver %s > /var/lib/torque/mom_priv/config' 
                sudo bash -c 'echo \$logevent 255 >> /var/lib/torque/mom_priv/config' 
                ''' % (self.processor_type,pbsserver_name)
            elif "5." in self.distro:
                batch_commands = '''
               
                echo "Adding epel repository"
                wget http://dl.fedoraproject.org/pub/epel/5/%s/epel-release-5-4.noarch.rpm
                sudo rpm -Uvh epel-release-5*.noarch.rpm
                echo "Installing TORQUE from rpm: "
                sudo yum -y update
                sudo yum -y install torque-client torque-mom munge
                
                sudo bash -c '[ -f /etc/munge/munge.key ] || /usr/sbin/create-munge-key'
                sudo bash -c 'echo \$pbsserver %s > /var/lib/torque/mom_priv/config' 
                sudo bash -c 'echo \$logevent 255 >> /var/lib/torque/mom_priv/config' 
                sudo /etc/init.d/munge start
                ''' % (self.processor_type,pbsserver_name)
            else:
                print "TODO: Torque for "+self.distro
                batch_commands = ""
            self.addBatchCommand(batch_commands) 
        #print batch_commands  
        self.runAllBatchCommands()
            
    #-------------------------------
    #
    # restartTorqueServer
    #
    # When installed from the package manager, torque doesn't start with correct options. This restarts it.
    #
    #-------------------------------


    def restartTorqueServer(self):
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            self.runSingleCommandAsBatch("sudo /etc/init.d/torque-server restart")
            self.runSingleCommandAsBatch("sudo /etc/init.d/torque-scheduler restart")
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
            self.runSingleCommandAsBatch("sudo /etc/init.d/munge stop")
            self.runSingleCommandAsBatch("sudo /etc/init.d/munge start")            
            self.runSingleCommandAsBatch("sudo /etc/init.d/pbs_server stop")
            self.runSingleCommandAsBatch("sudo /etc/init.d/pbs_server start")
            self.runSingleCommandAsBatch("sudo /etc/init.d/pbs_sched stop")
            self.runSingleCommandAsBatch("sudo /etc/init.d/pbs_sched start")
            

        
    #-------------------------------
    #
    # restartTorqueMom
    #
    # When installed from the package manager, torque doesn't start with correct options. This restarts it.
    #
    #-------------------------------

    
    def restartTorqueMom(self):
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            self.runSingleCommandAsBatch("sudo /etc/init.d/torque-mom restart")
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
              
            self.runSingleCommandAsBatch("sudo /etc/init.d/pbs_mom restart")


    


    #-------------------------------
    #
    # installRequiredSoftware
    #
    # This installs all the prerequisites for building and testing OrangeFS from the package management system
    #
    #-------------------------------


    def installRequiredSoftware(self):
        
        
        if "ubuntu" in self.distro.lower() or "mint" in self.distro.lower() or "debian" in self.distro.lower():
            batch_commands = '''
                sudo DEBIAN_FRONTEND=noninteractive apt-get update > /dev/null
                #documentation needs to be updated. linux-headers needs to be added for ubuntu!
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q openssl gcc g++ gfortran flex bison libssl-dev linux-source perl make linux-headers-`uname -r` zip subversion automake autoconf  pkg-config rpm patch libuu0 libuu-dev libuuid1 uuid uuid-dev uuid-runtime < /dev/null
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q libfuse2 fuse-utils libfuse-dev < /dev/null
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q autofs nfs-kernel-server rpcbind nfs-common nfs-kernel-server < /dev/null
                # needed for Ubuntu 10.04
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q linux-image < /dev/null
                # will fail on Ubuntu 10.04. Run separately to not break anything
                sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q fuse < /dev/null
                #sudo DEBIAN_FRONTEND=noninteractive apt-get install -yu avahi-autoipd  avahi-dnsconfd  avahi-utils avahi-daemon    avahi-discover  avahi-ui-utils </dev/null
                sudo apt-get clean

                #prepare source
                SOURCENAME=`find /usr/src -name "linux-source*" -type d -prune -printf %f`
                cd /usr/src/${SOURCENAME}
                sudo tar -xjf ${SOURCENAME}.tar.bz2  &> /dev/null
                cd ${SOURCENAME}/
                sudo cp /boot/config-`uname -r` .config
                sudo make oldconfig &> /dev/null
                sudo make prepare &>/dev/null
                if [ ! -f /lib/modules/`uname -r`/build/include/linux/version.h ]
                then
                sudo ln -s include/generated/uapi/version.h /lib/modules/`uname -r`/build/include/linux/version.h
                fi
                sudo /sbin/modprobe -v fuse
                sudo chmod a+x /bin/fusermount
                sudo chmod a+r /etc/fuse.conf
                sudo rm -rf /opt
                sudo ln -s /mnt /opt
                sudo chmod -R a+w /mnt
                sudo service cups stop
                sudo service sendmail stop
                sudo service rpcbind restart
                sudo service nfs-kernel-server restart
                

            '''
            self.addBatchCommand(batch_commands)
            
            
        elif "suse" in self.distro.lower():
            batch_commands = '''
            # prereqs should be installed as part of the image. Thanx SuseStudio!
            #zypper --non-interactive install gcc gcc-c++ flex bison libopenssl-devel kernel-source kernel-syms kernel-devel perl make subversion automake autoconf zip fuse fuse-devel fuse-libs sudo nano openssl
            sudo zypper --non-interactive patch libuuid1 uuid-devel
            

            cd /usr/src/linux-`uname -r | sed s/-[\d].*//`
            sudo cp /boot/config-`uname -r` .config
            sudo make oldconfig &> /dev/null
            sudo make modules_prepare &>/dev/null
            sudo make prepare &>/dev/null
            sudo ln -s /lib/modules/`uname -r`/build/Module.symvers /lib/modules/`uname -r`/source
            if [ ! -f /lib/modules/`uname -r`/build/include/linux/version.h ]
            then
            sudo ln -s include/generated/uapi/version.h /lib/modules/`uname -r`/build/include/linux/version.h
            fi
            sudo modprobe -v fuse
            sudo chmod a+x /bin/fusermount
            sudo chmod a+r /etc/fuse.conf
            #sudo mkdir -p /opt
            sudo rm -rf /opt
            sudo ln -s /mnt /opt
            sudo chmod -R a+w /opt



            '''
            self.addBatchCommand(batch_commands)
        elif "centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower() or "fedora" in self.distro.lower():
            
            batch_commands = '''
                echo "Installing prereqs via yum..."
                sudo yum -y install gcc gcc-c++ gcc-gfortran openssl fuse flex bison openssl-devel kernel-devel-`uname -r` kernel-headers-`uname -r` perl make subversion automake autoconf zip fuse fuse-devel fuse-libs wget patch bzip2 libuuid libuuid-devel uuid uuid-devel
                sudo yum -y install nfs-utils nfs-utils-lib nfs-kernel nfs-utils-clients rpcbind libtool libtool-ltdl 
                sudo /sbin/modprobe -v fuse
                sudo chmod a+x /bin/fusermount
                sudo chmod a+r /etc/fuse.conf
                #sudo mkdir -p /opt
                #link to use additional space in /mnt drive
                sudo rm -rf /opt
                sudo ln -s /mnt /opt
                sudo chmod -R a+w /mnt
                sudo chmod -R a+w /opt
                sudo service cups stop
                sudo service sendmail stop
                sudo service rpcbind start
                sudo service nfs restart

            '''
            self.addBatchCommand(batch_commands)

        # db4 is built from scratch for all systems to have a consistant version.
        batch_commands = '''
        
        if [ ! -d %s ]
        then
            cd ~
            wget -q http://devorange.clemson.edu/pvfs/db-4.8.30.tar.gz
            tar zxf db-4.8.30.tar.gz &> /dev/null
            cd db-4.8.30/build_unix
            echo "Configuring Berkeley DB 4.8.30..."
            ../dist/configure --prefix=%s &> db4conf.out
            echo "Building Berkeley DB 4.8.30..."
            make &> db4make.out
            echo "Installing Berkeley DB 4.8.30 to %s..."
            make install &> db4install.out
        fi
        exit
        exit
        ''' % (self.db4_dir,self.db4_dir,self.db4_dir)
        
        self.db4_lib_dir = self.db4_dir+"/lib"
        self.addBatchCommand(batch_commands)
        self.runAllBatchCommands()
        self.setEnvironmentVariable("LD_LIBRARY_PATH","%s:$LD_LIBRARY_PATH" % self.db4_lib_dir)

    #-------------------------------
    #
    # copyOFSSourceFromSVN
    #
    # This copies the source from an SVN branch
    #
    #-------------------------------


    def copyOFSSourceFromSVN(self,svnurl,dest_dir,svnusername,svnpassword):
    
        output = []
        self.ofs_branch = os.path.basename(svnurl)
    
        #export svn by default. This merely copies from SVN without allowing update
        svn_options = ""
        svn_action = "export --force"
        
        # use the co option if we have a username and password
        if svnusername != "" and svnpassword != "":
            svn_options = "%s --username %s --password %s" % (svn_options, svnusername,svnpassword)
            svn_action = "co"
        
            
        
        
        print "svn %s %s %s" % (svn_action,svnurl,svn_options)
        self.changeDirectory(dest_dir)
        rc = self.runSingleCommand("svn %s %s %s" % (svn_action,svnurl,svn_options),output)
        if rc != 0:
            print "Could not export from svn"
            print output
            return rc
        else:
            self.ofs_source_location = "%s/%s" % (dest_dir.rstrip('/'),self.ofs_branch)
            print "svn exported to %s" % self.ofs_source_location
        
        
        return rc


    #-------------------------------
    #
    # install benchmarks
    #
    # This downloads and untars the thirdparty benchmarks
    #
    #-------------------------------


    def installBenchmarks(self,tarurl="http://devorange.clemson.edu/pvfs/benchmarks-20121017.tar.gz",dest_dir="",configure_options="",make_options="",install_options=""):
        if dest_dir == "":
            dest_dir = "/home/%s/" % self.current_user
        print "Installing benchmarks from "+tarurl
        tarfile = os.path.basename(tarurl)
        output = []
        
        #make sure the directory is there
        self.runSingleCommand("mkdir -p "+dest_dir)
        self.changeDirectory(dest_dir)
        self.runSingleCommand("rm " + tarfile)
        rc = self.runSingleCommand("wget " + tarurl, output)
        if rc != 0:
            print "Could not download benchmarks"
            print output
            return rc
        tarflags = ""
        taridx = 0
    
        if ".tar.gz" in tarfile:
            tarflags = "zxf"
            taridx = tarfile.index(".tar.gz")
        elif ".tgz" in tarfile:
          tarflags = "zxf"
          taridx = tarfile.index(".tgz")
        elif ".tar.bz2" in tarfile:
          tarflags = "jxf"
          taridx = tarfile.index(".tar.bz2")
        elif ".tar" in tarfile:
          tarflags = "xf"
          taridx = tarfile.index(".tar")
        else:
          print "%s Not a tarfile" % tarurl
          return 1
    
        tardir = tarfile[:taridx]
        rc = self.runSingleCommand("tar %s %s" % (tarflags, tarfile))
        #print self.runSingleCommandBacktick("ls %s" % dest_dir)
        if rc != 0:
            print "Could not untar benchmarks"
            print output
            return rc
        
        self.ofs_extra_tests_location = dest_dir+"/benchmarks" 
        #print "Extra tests location: "+self.ofs_extra_tests_location
        #print self.runSingleCommandBacktick("ls %s" % self.ofs_extra_tests_location)
        return 0
    
    
    
    #-------------------------------
    #
    # makeFromTarFile
    #
    # This is a generic function to ./configure, make, make install a tarball
    #
    #-------------------------------

    def makeFromTarFile(self,tarurl,dest_dir,configure_options="",make_options="",install_options=""):
        tarfile = os.path.basename(tarurl)
        self.changeDirectory(dest_dir)
        self.runSingleCommand("rm " + tarfile)
        self.runSingleCommand("wget " + tarurl)
        tarflags = ""
        taridx = 0
    
        if ".tar.gz" in tarfile:
            tarflags = "zxf"
            taridx = tarfile.index(".tar.gz")
        elif ".tgz" in tarfile:
          tarflags = "zxf"
          taridx = tarfile.index(".tgz")
        elif ".tar.bz2" in tarfile:
          tarflags = "jxf"
          taridx = tarfile.index(".tar.bz2")
        elif ".tar" in tarfile:
          tarflags = "xf"
          taridx = tarfile.index(".tar")
        else:
          print "%s Not a tarfile" % tarurl
          return 1
        
        tardir = tarfile[:taridx]
        self.runSingleCommand("tar %s %s" % (tarflags, tarfile))
        self.changeDirectory(tardir)
        self.runSingleCommand("./prepare")
        self.runSingleCommand("./configure "+ configure_options)
        
        self.runSingleCommand("make "+ make_options)
        self.runSingleCommand("make install "+install_options)
    
    
    #-------------------------------
    #
    # copyOFSSourceFromRemoteTarball
    #
    # This downloads the source from a remote tarball. Several forms are 
    # supported
    #
    #-------------------------------
    
    
    def copyOFSSourceFromRemoteTarball(self,tarurl,dest_dir):
    
        tarfile = os.path.basename(tarurl)
        #make sure the directory is there
        self.runSingleCommand("mkdir -p "+dest_dir)
        self.changeDirectory(dest_dir)
        self.runSingleCommand("rm " + tarfile)
        output = []
        rc = self.runSingleCommand("wget " + tarurl,output)
        if rc != 0:
            print "Could not download OrangeFS"
            print output
            return rc
        tarflags = ""
        taridx = 0
        
        if ".tar.gz" in tarfile:
          tarflags = "zxf"
          taridx = tarfile.index(".tar.gz")
        elif ".tgz" in tarfile:
          tarflags = "zxf"
          taridx = tarfile.index(".tgz")
        elif ".tar.bz2" in tarfile:
          tarflags = "jxf"
          taridx = tarfile.index(".tar.bz2")
        elif ".tar" in tarfile:
          tarflags = "xf"
          taridx = tarfile.index(".tar")
        else:
          print "%s Not a tarfile" % tarurl
          return 1
        
        rc = self.runSingleCommand("tar %s %s" % (tarflags, tarfile),output)
        if rc != 0:
            print "Could not untar OrangeFS"
            print output
            return rc
        
        #remove the extension from the tarfile for the directory. That is the assumption
        self.ofs_branch = tarfile[:taridx]
        
        self.ofs_source_location = "%s/%s" % (dest_dir.rstrip('/'),self.ofs_branch)
        # Change directory /tmp/user/
        # source_location = /tmp/user/source
        return rc
  
    #-------------------------------
    #
    # copyOFSSourceFromDirectory
    #
    # This copies the source from a local directory
    #
    #-------------------------------


    def copyOFSSourceFromDirectory(self,directory,dest_dir):
        rc = 0
        if directory != dest_dir:
            rc = self.copyLocal(directory,dest_dir,True)
        self.ofs_source_location = dest_dir
        dest_list = os.path.basename(dest_dir)
        self.ofs_branch = dest_list[-1]
        return rc
    
    #-------------------------------
    #
    # copyOFSSourceFromSVN
    #
    # This copies the source from a remote directory
    #
    #-------------------------------

      
    def copyOFSSourceFromRemoteNode(self,source_node,directory,dest_dir):
        #implemented in subclass
        return 0
        
      
      
    def copyOFSSource(self,resource_type,resource,dest_dir,username="",password=""):
        # Make directory dest_dir
        rc = self.runSingleCommand("mkdir -p %s" % dest_dir)
        if rc != 0:
            print "Could not mkdir -p %s" %dest_dir
            return rc
          
        
        # ok, now what kind of resource do we have here?
        # switch on resource_type
        #
        
        #print "Copy "+ resource_type+ " "+ resource+ " "+dest_dir
        
        if resource_type == "SVN":
          rc = self.copyOFSSourceFromSVN(resource,dest_dir,username,password)
        elif resource_type == "TAR":
          rc = self.copyOFSSourceFromRemoteTarball(resource,dest_dir)
        elif resource_type == "REMOTEDIR":
          self.copyOFSSourceFromRemoteNode(directory,dest_dir)
        elif resource_type == "LOCAL":
            # Must be "pushed" from local node to current node.
            # Get around this by copying to the buildnode, then resetting type.
            pass
        elif resource_type == "BUILDNODE":
          # else local directory
          rc = self.copyOFSSourceFromDirectory(resource,dest_dir)
        else:
          print "Resource type %s not supported!\n" % resource_type
          return -1
        
        
        return rc
        
    #-------------------------------
    #
    # configureOFSSource
    #
    # This prepares the OrangeFS source and runs the configure command.
    #
    #-------------------------------

      
    def configureOFSSource(self,
        build_kmod=True,
        enable_strict=False,
        enable_fuse=False,
        enable_shared=False,
        ofs_prefix="/opt/orangefs",
        db4_prefix="/opt/db4",
        security_mode=None,
        ofs_patch_files=[],
        configure_opts="",
        debug=False):
    
        
        # Change directory to source location.
        self.changeDirectory(self.ofs_source_location)
        # Run prepare.
        output = []
        #cwd = self.runSingleCommandBacktick("pwd")
        #print cwd
        #ls = self.runSingleCommandBacktick("ls -l")
        #print ls
        print ofs_patch_files
        for patch in ofs_patch_files:
            
            print "Patching: patch -c -p1 < %s" % patch
            rc = self.runSingleCommand("patch -c -p1 < %s" % patch)
            if rc != 0:
                print "Patch Failed!"
        
        rc = self.runSingleCommand("./prepare",output)
        if rc != 0:
            print self.ofs_source_location+"/prepare failed!" 
            print output
            return rc
        
        #sanity check for OFS prefix
        rc = self.runSingleCommand("mkdir -p "+ofs_prefix)
        if rc != 0:
            print "Could not create directory "+ofs_prefix
            ofs_prefix = "/home/%s/orangefs" % self.current_user
            print "Using default %s" % ofs_prefix
        else:
            self.runSingleCommand("rmdir "+ofs_prefix)
            
        
        # get the kernel version if it has been updated
        self.kernel_version = self.runSingleCommandBacktick("uname -r")
        
        self.kernel_source_location = "/lib/modules/%s" % self.kernel_version
        
        
        configure_opts = configure_opts+" --prefix=%s --with-db=%s" % (ofs_prefix,db4_prefix)
        
        # various configure options
        
        if build_kmod == True:
            if "suse" in self.distro.lower():
                configure_opts = "%s --with-kernel=%s/source" % (configure_opts,self.kernel_source_location)
            else:
                configure_opts = "%s --with-kernel=%s/build" % (configure_opts,self.kernel_source_location)
        
        if enable_strict == True:
            # should check gcc version, but am too lazy for that. Will work on gcc > 4.4
            # gcc_ver = self.runSingleCommandBacktick("gcc -v 2>&1 | grep gcc | awk {'print \$3'}")
            
            # won't work for rhel 5 based distros, gcc is too old.
            if ("centos" in self.distro.lower() or "scientific linux" in self.distro.lower() or "red hat" in self.distro.lower()) and " 5." in self.distro:
                pass
            else:
                configure_opts = configure_opts+" --enable-strict"

        if enable_shared == True:
            configure_opts = configure_opts+" --enable-shared"

        if enable_fuse == True:
            configure_opts = configure_opts+" --enable-fuse"
        
        
        if security_mode == None:
            # no security mode, ignore
            # must come first to prevent exception
            pass
        elif security_mode.lower() == "key":
            configure_opts = configure_opts+" --enable-security-key"
        elif security_mode.lower() == "cert":
            configure_opts = configure_opts+" --enable-security-cert"
        
        rc = self.runSingleCommand("./configure %s" % configure_opts, output)
        
        # did configure run correctly?
        if rc == 0:
            # set the OrangeFS installation location to the prefix.
            self.ofs_installation_location = ofs_prefix
        else:
            print "Configuration of OrangeFS at %s Failed!" % self.ofs_source_location
            print output
            

        return rc
    
    #-------------------------------
    #
    # checkMount
    #
    # This looks to see if a given mount_point is mounted.
    # return == 0 => mounted
    # return != 0 => not mounted
    #
    #-------------------------------

        
    def checkMount(self,mount_point=None,output=[]):
        if mount_point == None:
            mount_point = self.ofs_mount_point
        mount_check = self.runSingleCommand("mount | grep %s" % mount_point,output)
        '''    
        if mount_check == 0:
            print "OrangeFS mount found: "+output[1]
        else:
            print "OrangeFS mount not found!"
            print output
        '''
        return mount_check

    def getAliasesFromConfigFile(self,config_file_name):
        
        pass
        
        
    #-------------------------------
    #
    # makeOFSSource
    #
    # This makes the OrangeFS source
    #
    #-------------------------------

    
    
    def makeOFSSource(self,make_options=""):
        # Change directory to source location.
        self.changeDirectory(self.ofs_source_location)
        output = []
        rc = self.runSingleCommand("make clean")
        rc = self.runSingleCommand("make "+make_options, output)
        if rc != 0:
            print "Build (make) of of OrangeFS at %s Failed!" % self.ofs_source_location
            print output
            return rc
        
        rc = self.runSingleCommand("make kmod",output)
        if rc != 0:
            print "Build (make) of of OrangeFS-kmod at %s Failed!" % self.ofs_source_location
            print output
            
        return rc
    
    #-------------------------------
    #
    # getKernelVerions
    #
    # wrapper for uname -r
    #
    #-------------------------------



    def getKernelVersion(self):
        #if self.kernel_version != "":
        #  return self.kernel_version
        return self.runSingleCommand("uname -r")

    #-------------------------------
    #
    # installOFSSource
    #
    # This looks to see if a given mount_point is mounted
    #
    #-------------------------------
      
    def installOFSSource(self,install_options="",install_as_root=False):
        self.changeDirectory(self.ofs_source_location)
        output = []
        if install_as_root == True:
            rc = self.runSingleCommandAsBatch("sudo make install",output)
        else:
            rc = self.runSingleCommand("make install",output)
        
        if rc != 0:
            
            print "Could not install OrangeFS from %s to %s" % (self.ofs_source_location,self.ofs_installation_location)
            print output
            return rc
        self.runSingleCommand("make kmod_install kmod_prefix=%s" % self.ofs_installation_location,output)
        if rc != 0:
            print "Could not install OrangeFS from %s to %s" % (self.ofs_source_location,self.ofs_installation_location)
            print output
        
        return rc

    #-------------------------------
    #
    # installOFSTests
    #
    # this installs the OrangeFS test programs
    #
    #-------------------------------

    
        
    def installOFSTests(self,configure_options=""):
        
        output = []
        
        if configure_options == "":
            configure_options = "--with-db=%s --prefix=%s" % (self.db4_dir,self.ofs_installation_location)
        
 
        
        self.changeDirectory("%s/test" % self.ofs_source_location)
        rc = self.runSingleCommand("./configure %s"% configure_options)
        if rc != 0:
            print "Could not configure OrangeFS tests"
            print output
            return rc
        
        rc = self.runSingleCommand("make all")
        if rc != 0:
            print "Could not build (make) OrangeFS tests"
            print output
            return rc
   
        rc = self.runSingleCommand("make install")
        if rc != 0:
            print "Could not install OrangeFS tests"
            print output
        return rc
   

    #-------------------------------
    #
    # generatePAVConf
    #
    # generates the pav.conf file for MPI testing
    #
    #-------------------------------


    def generatePAVConf(self,**kwargs):
        
        pav_conf = kwargs.get("pav_conf")
        if pav_conf == None:
            pav_conf = "/home/%s/pav.conf" % self.current_user
        
        keep_conf = kwargs.get("keep_conf")    
        if keep_conf != None:
            return pav_conf
        
        file = open("/tmp/pav.conf","w+")
        
        file.write("SRCDIR_TOP=%s\n" % self.ofs_source_location)
        file.write("BUILDDIR=%s\n" % self.ofs_source_location)
        
        nodefile = self.runSingleCommandBacktick("echo REAL LIST OF NODES HERE")
        
        file.write("NODEFILE=%s\n" % nodefile)
        file.write("IONCOUNT=1\n")
        file.write("METACOUNT=1\n")
        file.write("UNIQUEMETA=0\n")
        # create this directory on all nodes
        file.write("WORKINGDIR=/tmp/pvfs-pav-working\n")
        file.write("PROTOCOL=tcp\n")
        file.write("PVFSTCCPORT=%s\n" % self.ofs_tcp_port)
        file.write("STORAGE=$WORKINGDIR/storage\n")
        file.write("SERVERLOG=$WORKINGDIR/log\n")
        file.write("MOUNTPOINT=%s\n" % self.ofs_mount_point)
        file.write("BINDIR=$WORKINDIR/bin\n")
        file.write("RCMDPROG=ssh -i %s\n" % self.sshNodeKeyFile)
        file.write("RCPPROG=scp -i %s\n" % self.sshNodeKeyFile)
        file.write("RCMDPROG_ROOT=ssh -l root -i %s\n" % self.sshNodeKeyFile)
        file.write("PROGROOT=$SRCDIR_TOP/test/common/pav\n")
        file.write("SERVER=%s/sbin/pvfs2-server\n" % self.ofs_installation_location)
        file.write("PINGPROG=%s/bin/pvfs2-ping\n" % self.ofs_installation_location)
        file.write("GENCONFIG=%s/bin/pvfs2-genconfig\n" % self.ofs_installation_location)
        file.write("COPYBINS=1\n")
        file.write("TROVESYNC=1\n")
        file.write("MOUNT_FS=0\n")
        file.write("KERNEL_KVER=%s\n" % self.kernel_version)
        file.write("PVFS_KMOD=%s/lib/modules/%s/kernel/fs/pvfs2/pvfs2.ko\n" % (self.ofs_installation_location,self.kernel_version))
        file.write("PVFS_CLIENT=%s/sbin/pvfs2-client\n" % self.ofs_installation_location)
        file.write("COMPUTENODES_LAST=1\n")
        file.close()
        
        return pav_conf
        
        #============================================================================
        #
        # OFSServerFunctions
        #
        # These functions implement functionality for an OrangeFS server
        #
        #=============================================================================

    #-------------------------------
    #
    # opyOFSInstallationToNode
    #
    # this copies an entire OrangeFS installation from the current node to destinationNode.
    # Also sets the ofs_installation_location and ofs_branch on the destination
    #
    #-------------------------------


    def copyOFSInstallationToNode(self,destinationNode):
        rc = self.copyToRemoteNode(self.ofs_installation_location+"/", destinationNode, self.ofs_installation_location, True)
        destinationNode.ofs_installation_location = self.ofs_installation_location
        destinationNode.ofs_branch =self.ofs_branch
        # TODO: Copy ofs_conf_file, don't just link
        #rc = self.copyToRemoteNode(self.ofs_conf_file+"/", destinationNode, self.ofs_conf_file, True)
        destinationNode.ofs_conf_file =self.ofs_conf_file
        return rc
       
    def copyMpich2InstallationToNode(self,destinationNode):
        rc = self.copyToRemoteNode(self.mpich2_installation_location+"/", destinationNode, self.mpich2_installation_location, True)
        rc = self.copyToRemoteNode(self.mpich2_source_location+"/", destinationNode, self.mpich2_source_location, True)
        destinationNode.mpich2_installation_location = self.mpich2_installation_location
        destinationNode.mpich2_source_location = self.mpich2_source_location
        return rc
    #-------------------------------
    #
    # configureOFSServer
    #
    # This function runs the configuration programs and puts the result in self.ofs_installation_location/etc/orangefs.conf 
    #
    #-------------------------------
      
    
       
    def configureOFSServer(self,ofs_hosts_v,ofs_fs_name,configuration_options="",ofs_source_location="",ofs_storage_location="",ofs_conf_file=None,security=None):
        
            
        self.ofs_fs_name=ofs_fs_name
        
        self.changeDirectory(self.ofs_installation_location)
        
        if ofs_storage_location == "":
            self.ofs_storage_location  = self.ofs_installation_location + "/data"
        else:
            self.ofs_storage_location = self.ofs_storage_location
           
        # ofs_hosts is a list of ofs hosts separated by white space.
        ofs_host_str = ""
        
        # Add each ofs host to the string of hosts.
        for ofs_host in ofs_hosts_v:
           ofs_host_str = ofs_host_str + ofs_host.host_name + ":3396,"
        
        #strip the trailing comma
        ofs_host_str = ofs_host_str.rstrip(',')

        #implement the following command
        '''
        INSTALL-pvfs2-${CVS_TAG}/bin/pvfs2-genconfig fs.conf \
            --protocol tcp \
            --iospec="${MY_VFS_HOSTS}:3396" \
            --metaspec="${MY_VFS_HOSTS}:3396"  \
            --storage ${PVFS2_DEST}/STORAGE-pvfs2-${CVS_TAG} \
            $sec_args \
            --logfile=${PVFS2_DEST}/pvfs2-server-${CVS_TAG}.log --quiet
        ''' 
      
        security_args = ""
        if security == None:
            pass
        elif security.lower() == "key":
            print "Configuring key based security"
            security_args = "--securitykey --serverkey=%s/etc/orangefs-serverkey.pem --keystore=%s/etc/orangefs-keystore" % (self.ofs_installation_location,self.ofs_installation_location)
        elif security.lower() == "cert":
            print "Certificate based security not yet supported by OFSTest."
            pass
            
        self.runSingleCommand("mkdir -p %s/etc" % self.ofs_installation_location)
        if configuration_options == "":
            genconfig_str="%s/bin/pvfs2-genconfig %s/etc/orangefs.conf --protocol tcp --iospec=\"%s\" --metaspec=\"%s\" --storage=%s %s --logfile=%s/pvfs2-server-%s.log --quiet" % (self.ofs_installation_location,self.ofs_installation_location,ofs_host_str,ofs_host_str,self.ofs_storage_location,security_args,self.ofs_installation_location,self.ofs_branch)
        else:
            genconfig_str="%s/bin/pvfs2-genconfig %s/etc/orangefs.conf %s --quiet" % (self.ofs_installation_location,self.ofs_installation_location,configuration_options)
        
        print "Generating orangefs.conf "+ genconfig_str
        # run genconfig
        output = []
        rc = self.runSingleCommand(genconfig_str,output)
        if rc != 0:
            print "Could not generate orangefs.conf file."
            print output
            return rc
        
        # do we need to copy the file to a new location?
        if ofs_conf_file == None:
            self.ofs_conf_file = self.ofs_installation_location+"/etc/orangefs.conf"
        else:
            rc = self.copyLocal(self.ofs_installation_location+"/etc/orangefs.conf",ofs_conf_file,False)
            if rc != 0:
                print "Could not copy orangefs.conf file to %s. Using %s/etc/orangefs.conf" % (ofs_conf_file,self.ofs_installation_location)
                self.ofs_conf_file = self.ofs_installation_location+"/etc/orangefs.conf"
            else:
                self.ofs_conf_file = ofs_conf_file
        
        # Now set the fs name
        self.ofs_fs_name = self.runSingleCommandBacktick("grep Name %s | awk {'print \$2'}" % self.ofs_conf_file)
        
        return rc
        
    #-------------------------------
    #
    # startOFSServer
    #
    # This function starts the orangefs server
    #
    #-------------------------------
        
      
    def startOFSServer(self,run_as_root=False):
        
        output = []
        self.changeDirectory(self.ofs_installation_location)
        #print self.runSingleCommand("pwd")
        # initialize the storage
        
        '''
        Change the following shell command to python
        
        for alias in `grep 'Alias ' fs.conf | grep ${HOSTNAME} | cut -d ' ' -f 2`; do
            ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/sbin/pvfs2-server \
                -p `pwd`/pvfs2-server-${alias}.pid \
                -f fs.conf -a $alias
            ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/sbin/pvfs2-server \
                -p `pwd`/pvfs2-server-${alias}.pid  \
                fs.conf $server_conf -a $alias
        '''
        
        # This is sloppy, but should work. Ideally should reimplement in python, but don't want to reinvent the wheel.
        print "Attempting to start OFSServer for host %s" % self.host_name
        
        #self.runSingleCommand("ls -l %s" % self.ofs_installation_location)
        #print "Installation location is %s" % self.ofs_installation_location
        #print "Running command \"grep 'Alias ' %s/etc/orangefs.conf | grep %s | cut -d ' ' -f 2\"" % (self.ofs_installation_location,self.host_name)

        # need to get the alias list from orangefs.conf file
        if self.alias_list == None:
            self.alias_list = self.getAliasesFromConfigFile(self.ofs_conf_file)

        #aliases = self.runSingleCommandBacktick('grep "Alias " %s/etc/orangefs.conf | grep %s | cut -d " " -f 2' % (self.ofs_installation_location,self.host_name))
        
        if len(self.alias_list) == 0:
            print "Could not find any aliases in %s/etc/orangefs.conf" % self.ofs_installation_location
            return -1

        # for all the aliases in the file
        for alias in self.alias_list:
            # if the alias is for THIS host
            if self.host_name in alias:
                
                #self.runSingleCommand("rm -rf %s" % self.ofs_storage_location)
                #self.runSingleCommand("mkdir -p %s" % self.ofs_storage_location)
                #self.runSingleCommand("cat %s/etc/orangefs.conf")
                
                # create storage space for the server
                rc = self.runSingleCommand("%s/sbin/pvfs2-server -p %s/pvfs2-server-%s.pid -f %s/etc/orangefs.conf -a %s" % ( self.ofs_installation_location,self.ofs_installation_location,self.host_name,self.ofs_installation_location,alias),output)
                if rc != 0:
                    # If storage space is already there, creating it will fail. Try deleting and recreating.
                    rc = self.runSingleCommand("%s/sbin/pvfs2-server -p %s/pvfs2-server-%s.pid -r %s/etc/orangefs.conf -a %s" % ( self.ofs_installation_location,self.ofs_installation_location,self.host_name,self.ofs_installation_location,alias),output)
                    rc = self.runSingleCommand("%s/sbin/pvfs2-server -p %s/pvfs2-server-%s.pid -f %s/etc/orangefs.conf -a %s" % ( self.ofs_installation_location,self.ofs_installation_location,self.host_name,self.ofs_installation_location,alias),output)
                    if rc != 0:
                        print "Could not create OrangeFS storage space"
                        print output
                        return rc
              
                
                # Are we running this as root? 
                prefix = "" 
                if run_as_root == True:
                    prefix = "sudo LD_LIBRARY_PATH=%s:%s/lib" % (self.db4_lib_dir,self.ofs_installation_location)
                    
                    
                server_start = "%s %s/sbin/pvfs2-server -p %s/pvfs2-server-%s.pid %s/etc/orangefs.conf -a %s" % (prefix,self.ofs_installation_location,self.ofs_installation_location,self.host_name,self.ofs_installation_location,alias)
                print server_start
                rc = self.runSingleCommandAsBatch(server_start,output)
                
                #if rc != 0:
                #    print "Could not start OrangeFS server"
                #    print output
                #    return rc

                #self.runAllBatchCommands()
                # give the servers 15 seconds to get running
                print "Starting OrangeFS servers..."
                time.sleep(15)
    
        #   print "Checking to see if OrangeFS servers are running..."
        #   running = self.runSingleCommand("ps aux | grep pvfs2")
        #      print running
          
        #Now set up the pvfs2tab_file
        self.ofs_mount_point = "/tmp/mount/orangefs"
        self.runSingleCommand("mkdir -p "+ self.ofs_mount_point)
        self.runSingleCommand("mkdir -p %s/etc" % self.ofs_installation_location)
        self.runSingleCommand("echo \"tcp://%s:3396/%s %s pvfs2 defaults 0 0\" > %s/etc/orangefstab" % (self.host_name,self.ofs_fs_name,self.ofs_mount_point,self.ofs_installation_location))
        self.setEnvironmentVariable("PVFS2TAB_FILE",self.ofs_installation_location + "/etc/orangefstab")
       
        # set the debug mask
        self.runSingleCommand("%s/bin/pvfs2-set-debugmask -m %s \"all\"" % (self.ofs_installation_location,self.ofs_mount_point))
       
        return 0
        
    def stopOFSServer(self):
        
        # read the file from the .pid file.
        # kill the pid
        #self.runSingleCommand("for pidfile in %s/pvfs2-server*.pid ; do if [ -f $pidfile ] ; then kill -9 `cat $pidfile`; fi; done")
        self.runSingleCommand("killall -s 9 pvfs2-server")
        
        #ofs_servers = self.stopOFSServer("ps aux | grep pvfs2-server"
        
        # if it's still alive, use kill -9
        
    #============================================================================ 
    #
    # OFSClientFunctions
    #
    # These functions implement functionality for an OrangeFS client
    #
    #=============================================================================
    
    #-------------------------------
    #
    # installKernelModule
    #
    # This function inserts the kernel module into the kernel
    #
    #-------------------------------

    def installKernelModule(self):
        
        # Installing Kernel Module is a root task, therefore, it must be done via batch.
        # The following shell commands are implemented in Python:
        '''
        sudo /sbin/insmod ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/lib/modules/`uname -r`/kernel/fs/pvfs2/pvfs2.ko &> pvfs2-kernel-module.log
        sudo /sbin/lsmod >> pvfs2-kernel-module.log
        '''
        self.addBatchCommand("sudo /sbin/insmod %s/lib/modules/%s/kernel/fs/pvfs2/pvfs2.ko &> pvfs2-kernel-module.log" % (self.ofs_installation_location,self.kernel_version))
        self.addBatchCommand("sudo /sbin/lsmod >> pvfs2-kernel-module.log")
        self.runAllBatchCommands()
        
     
    #-------------------------------
    #
    # startOFSClient
    #
    # This function starts the orangefs client
    #
    #-------------------------------
    def startOFSClient(self,security=None):
        # Starting the OFS Client is a root task, therefore, it must be done via batch.
        # The following shell command is implimented in Python
        '''
            keypath=""
        if [ $ENABLE_SECURITY ] ; then
            keypath="--keypath ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/etc/clientkey.pem"
        fi
        sudo ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/sbin/pvfs2-client \
            -p ${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/sbin/pvfs2-client-core \
            -L ${PVFS2_DEST}/pvfs2-client-${CVS_TAG}.log \
            $keypath
        sudo chmod 644 ${PVFS2_DEST}/pvfs2-client-${CVS_TAG}.logfile
        '''
        # TBD: Add security.
        keypath = ""
        if security==None:
            pass
        elif security.lower() == "key":
            keypath = "--keypath=%s/etc/pvfs2-clientkey.pem" % self.ofs_installation_location
        elif security.lower() == "cert":
            pass

        
        print "Starting pvfs2-client: "
        print "sudo LD_LIBRARY_PATH=%s:%s/lib PVFS2TAB_FILE=%s/etc/orangefstab  %s/sbin/pvfs2-client -p %s/sbin/pvfs2-client-core -L %s/pvfs2-client-%s.log %s" % (self.db4_lib_dir,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_branch,keypath)
        print ""
        
        # start the client 
        self.addBatchCommand("sudo LD_LIBRARY_PATH=%s:%s/lib PVFS2TAB_FILE=%s/etc/orangefstab  %s/sbin/pvfs2-client -p %s/sbin/pvfs2-client-core -L %s/pvfs2-client-%s.log %s" % (self.db4_lib_dir,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_installation_location,self.ofs_branch,keypath))
        # change the protection on the logfile to 644
        self.addBatchCommand("sudo chmod 644 %s/pvfs2-client-%s.log" % (self.ofs_installation_location,self.ofs_branch))
        self.runAllBatchCommands()
        #client_log = self.runSingleCommandBacktick("cat %s/pvfs2-client-%s.log" % (self.ofs_installation_location,self.ofs_branch))
        #print "Client log output"
        #print client_log
        
    #-------------------------------
    #
    # mountOFSFilesystem
    #
    # This function mounts OrangeFS via kernel module or fuse
    #
    #-------------------------------
      
    def mountOFSFilesystem(self,mount_fuse=False,mount_point=None):
        # Mounting the OFS Filesystem is a root task, therefore, it must be done via batch.
        # The following shell command is implimented in Python
        '''
            echo "Mounting pvfs2 service at tcp://${HOSTNAME}:3396/pvfs2-fs at mount_point $PVFS2_MOUNTPOINT"
        sudo mount -t pvfs2 tcp://${HOSTNAME}:3396/pvfs2-fs ${PVFS2_MOUNTPOINT}
        
        
        if [ $? -ne 0 ]
        then
            echo "Something has gone wrong. Mount failed."
        fi
        mount > allmount.log
        '''
        output = []
        
        # is the filesystem already mounted?
        rc = self.checkMount(output)
        if rc == 0:
            print "OrangeFS already mounted at %s" % output[1]
            return
        
        # where is this to be mounted?
        if mount_point != None:
            self.ofs_mount_point = mount_point
        elif self.ofs_mount_point == "":
            self.ofs_mount_point = "/tmp/mount/orangefs"

        # create the mount_point directory    
        self.runSingleCommand("mkdir -p %s" % self.ofs_mount_point)
        
        # mount with fuse
        if mount_fuse == True:
            print "Mounting OrangeFS service at tcp://%s:%s/%s at mount_point %s via fuse" % (self.host_name,self.ofs_tcp_port,self.ofs_fs_name,self.ofs_mount_point)
            self.runSingleCommand("%s/bin/pvfs2fuse %s -o fs_spec=tcp://%s:%s/%s -o nonempty" % (self.ofs_installation_location,self.ofs_mount_point,self.host_name,self.ofs_tcp_port,self.ofs_fs_name),output)
            #print output
            
        #mount with kmod
        else:
            print "Mounting OrangeFS service at tcp://%s:%s/%s at mount_point %s" % (self.host_name,self.ofs_tcp_port,self.ofs_fs_name,self.ofs_mount_point)
            self.addBatchCommand("sudo mount -t pvfs2 tcp://%s:%s/%s %s" % (self.host_name,self.ofs_tcp_port,self.ofs_fs_name,self.ofs_mount_point))
            self.runAllBatchCommands()
        
        print "Waiting 30 seconds for mount"            
        time.sleep(30)

    #-------------------------------
    #
    # mountOFSFilesystem
    #
    # This function unmounts OrangeFS. Works for both kmod and fuse
    #
    #-------------------------------
    
    def unmountOFSFilesystem(self):
        print "Unmounting OrangeFS mounted at " + self.ofs_mount_point
        self.addBatchCommand("sudo umount %s" % self.ofs_mount_point)
        self.addBatchCommand("sleep 10")

    #-------------------------------
    #
    # stopOFSClient
    #
    # This function stops the orangefs client
    #
    #-------------------------------
    

    def stopOFSClient(self):
        
        '''
            mount | grep -q $PVFS2_MOUNTPOINT && sudo umount $PVFS2_MOUNTPOINT
        ps -e | grep -q pvfs2-client && sudo killall pvfs2-client
        sleep 1
        /sbin/lsmod | grep -q pvfs2 && sudo /sbin/rmmod pvfs2
        # let teardown always succeed.  pvfs2-client might already be killed
        # and pvfs2 kernel module might not be loaded yet 
        '''
        self.unmountOFSFilesystem()
        print "Stopping pvfs2-client process"
        self.addBatchCommand("sudo killall pvfs2-client")
        self.addBatchCommand("sleep 10")
        self.addBatchCommand("sudo killall -s 9 pvfs2-client")
        self.addBatchCommand("sleep 2")
        #print "Removing pvfs2 kernel module."
        #self.addBatchCommand("sudo /sbin/rmmod pvfs2")
        #self.addBatchCommand("sleep 2")
        self.runAllBatchCommands()
        
    
    #-------------------------------
    #
    # setupMPIEnvironment
    #
    # This function sets up the environment for MPI testing
    #
    #-------------------------------
    
        
    def setupMPIEnvironment(self):
        
        home_dir = "/home/"+self.current_user
        self.setEnvironmentVariable("PAV_CONFIG","%s/pav.conf" % home_dir)

        # cluster environments need a few things available on a cluster-wide file
        # system: pav (which needs some pvfs2 programs), the mpi program, mpich2
        # (specifically mpd and tools )

        self.setEnvironmentVariable("CLUSTER_DIR","%s/nightly" % home_dir)
        self.addBatchCommand("mkdir -p ${CLUSTER_DIR}")
        self.addBatchCommand("rm -rf ${CLUSTER_DIR}/pav ${CLUSTER_DIR}/mpich2 ${CLUSTER_DIR}/pvfs2")
        self.addBatchCommand("cp -ar %s/test/common/pav ${CLUSTER_DIR}" % self.ofs_source_location)
        self.addBatchCommand("cp -ar %s/soft/mpich2 ${CLUSTER_DIR}" % self.ofs_source_location)
        self.addBatchCommand("cp -ar %s ${CLUSTER_DIR}/pvfs2" % self.ofs_installation_location)
        self.runAllBatchCommands()
    
    #-------------------------------
    #
    # installMPICH2
    #
    # This function installs mpich2 software
    #
    #-------------------------------
    

    def installMpich2(self,location=None):
        if location == None:
            location = "/home/%s/mpich2" % self.current_user
        
        mpich_version = "mpich-3.0.4"
            
        url = "http://devorange.clemson.edu/pvfs/%s.tar.gz" % mpich_version
        url = "wget"
        # just to make debugging less painful
        #[ -n "${SKIP_BUILDING_MPICH2}" ] && return 0
        #[ -d ${PVFS2_DEST} ] || mkdir ${PVFS2_DEST}
        self.runSingleCommand("mkdir -p "+location)
        tempdir = self.current_directory
        self.changeDirectory("/home/%s" % self.current_user)
        
        #wget http://www.mcs.anl.gov/research/projects/mpich2/downloads/tarballs/1.5/mpich2-1.5.tar.gz
        rc = self.runSingleCommand("wget --quiet %s" % url)
        #wget --passive-ftp --quiet 'ftp://ftp.mcs.anl.gov/pub/mpi/misc/mpich2snap/mpich2-snap-*' -O mpich2-latest.tar.gz
        if rc != 0:
            print "Could not download mpich from %s." % url
            self.changeDirectory(tempdir)
            return rc

        output = []
        self.runSingleCommand("tar xzf %s.tar.gz"% mpich_version)
        
        self.mpich2_source_location = "/home/%s/%s" % (self.current_user,mpich_version)
        self.changeDirectory(self.mpich2_source_location)
        #self.runSingleCommand("ls -l",output)
        #print output
        
        configure = '''
        ./configure -q --prefix=%s \
		--enable-romio --with-file-system=pvfs2 \
		--with-pvfs2=%s \
		--enable-g=dbg \
		 >mpich2config.log
        ''' % (location,self.ofs_installation_location)
        
        #wd = self.runSingleCommandBacktick("pwd")
        #print wd
        #print configure
        

        print "Configuring MPICH"
        rc = self.runSingleCommand(configure,output)
        
        if rc != 0:
            print "Configure of MPICH failed. rc=%d" % rc
            print output
            self.changeDirectory(tempdir)
            return rc
        
        print "Building MPICH"
        rc = self.runSingleCommand("make > mpich2make.log")
        if rc != 0:
            print "Make of MPICH failed."
            print output
            self.changeDirectory(tempdir)
            return rc

        print "Installing MPICH"
        rc = self.runSingleCommand("make install > mpich2install.log")
        if rc != 0:
            print "Install of MPICH failed."
            print output
            self.changeDirectory(tempdir)
            return rc
        
        print "Checking MPICH install"
        rc = self.runSingleCommand("make installcheck > mpich2installcheck.log")
        if rc != 0:
            print "Install of MPICH failed."
            print output
            self.changeDirectory(tempdir)
            return rc
        
        self.mpich2_installation_location = location 
        
        return 0
    

    def installOpenMPI(self,install_location=None,build_location=None):
        
        
        if install_location == None:
            install_location = "/opt/mpi"
        
        if build_location == None:
            build_location = install_location
        
        self.openmpi_version = "openmpi-1.6.5"
        url_base = "http://devorange.clemson.edu/pvfs/"
        url = url_base+self.openmpi_version+"-omnibond.tar.gz"

        patch_name = "openmpi.patch"
        patch_url = url_base+patch_name
        
        self.runSingleCommand("mkdir -p "+build_location)
        tempdir = self.current_directory
        self.changeDirectory(build_location)
        
        #wget http://www.mcs.anl.gov/research/projects/mpich2/downloads/tarballs/1.5/mpich2-1.5.tar.gz
        rc = self.runSingleCommand("wget --quiet %s" % url)
        #wget --passive-ftp --quiet 'ftp://ftp.mcs.anl.gov/pub/mpi/misc/mpich2snap/mpich2-snap-*' -O mpich2-latest.tar.gz
        if rc != 0:
            print "Could not download %s from %s." % (self.openmpi_version,url)
            self.changeDirectory(tempdir)
            return rc

        output = []
        self.runSingleCommand("tar xzf %s-omnibond.tar.gz"% self.openmpi_version)
        
        self.openmpi_source_location = "%s/%s" % (build_location,self.openmpi_version)
        self.changeDirectory(self.openmpi_source_location)
        rc = self.runSingleCommand("wget --quiet %s" % patch_url)


        # using pre-patched version. No longer needed.
        '''
        print "Patching %s" %self.openmpi_version
        rc = self.runSingleCommand("patch -p0 < %s" % patch_name,output)
        
        
        if rc != 0:
            print "Patching %s failed. rc=%d" % (self.openmpi_version,rc)
            print output
            self.changeDirectory(tempdir)
            return rc
        
        self.runSingleCommand("sed -i s/ADIOI_PVFS2_IReadContig/NULL/ ompi/mca/io/romio/romio/adio/ad_pvfs2/ad_pvfs2.c")
        self.runSingleCommand("sed -i s/ADIOI_PVFS2_IWriteContig/NULL/ ompi/mca/io/romio/romio/adio/ad_pvfs2/ad_pvfs2.c")
        '''
        #self.runSingleCommand("ls -l",output)
        #print output
        
        configure = './configure --prefix %s/openmpi --with-io-romio-flags=\'--with-pvfs2=%s --with-file-system=pvfs2+nfs\' >openmpiconfig.log' % (install_location,self.ofs_installation_location)
        
        #wd = self.runSingleCommandBacktick("pwd")
        #print wd
        #print configure
        

        print "Configuring %s" % self.openmpi_version
        rc = self.runSingleCommand(configure,output)
        
        if rc != 0:
            print "Configure of %s failed. rc=%d" % (self.openmpi_version,rc)
            print output
            self.changeDirectory(tempdir)
            return rc
        
        print "Making %s" % self.openmpi_version
        rc = self.runSingleCommand("make > openmpimake.log")
        if rc != 0:
            print "Make of %s failed."
            print output
            self.changeDirectory(tempdir)
            return rc

        print "Installing %s" % self.openmpi_version
        rc = self.runSingleCommand("make install > openmpiinstall.log")
        if rc != 0:
            print "Install of %s failed." % self.openmpi_version
            print output
            self.changeDirectory(tempdir)
            return rc
        
        #print "Checking MPICH install" % openmpi_version
        #rc = self.runSingleCommand("make installcheck > mpich2installcheck.log")
        #if rc != 0:
        #    print "Install of MPICH failed."
        #    print output
        #    self.changeDirectory(tempdir)
        #    return rc
        
        self.openmpi_installation_location = install_location+"/openmpi"
        
        
        return 0
    
        
    


        
        


    def findExistingOFSInstallation(self):
        # to find OrangeFS server, first finr the pvfs2-server file
        #ps -ef | grep -v grep| grep pvfs2-server | awk {'print $8'}
        output = []
        pvfs2_server = self.runSingleCommandBacktick("ps -f --no-heading -C pvfs2-server | awk {'print \$8'}")
        
        # We have <OFS installation>/sbin/pvfs2_server. Get what we want.
        (self.ofs_installation_location,sbin) = os.path.split(os.path.dirname(pvfs2_server))
        
        # to find OrangeFS conf file
        #ps -ef | grep -v grep| grep pvfs2-server | awk {'print $11'}
        self.ofs_conf_file = self.runSingleCommandBacktick("ps -f --no-heading -C pvfs2-server | awk {'print \$11'}")
        
        # to find url
        
        rc = self.runSingleCommandBacktick("ps -f --no-heading -C pvfs2-server | awk {'print \$13'}",output)
        #print output
        alias = output[1].rstrip()
        
        rc = self.runSingleCommandBacktick("grep %s %s | grep tcp: | awk {'print \$3'}" % (alias,self.ofs_conf_file),output )
        #print output
        url_base = output[1].rstrip()
        
        self.ofs_fs_name = self.runSingleCommandBacktick("grep Name %s | awk {'print \$2'}" % self.ofs_conf_file)
        
        # to find mount point
        # should be better than this.


        rc = self.runSingleCommand("mount | grep pvfs2 | awk { 'print \$2'}",output)
        if rc != 0:
            print "OrangeFS mount point not detected. Trying /tmp/mount/orangefs."
            self.ofs_mount_point = "/tmp/mount/orangefs"
        else: 
            self.ofs_mount_point = output[1].rstrip()
        
        # to find PVFS2TAB_FILE
        print "Looking for PVFS2TAB_FILE"
        rc = self.runSingleCommand("grep -l -r '%s/%s\s%s' %s 2> /dev/null" %(url_base,self.ofs_fs_name,self.ofs_mount_point,self.ofs_installation_location),output)
        if rc != 0:
            rc = self.runSingleCommand("grep -l -r '%s/%s\s%s' /etc 2> /dev/null" % (url_base,self.ofs_fs_name,self.ofs_mount_point),output)
        
        
        #if rc != 0:
        #    rc = self.runSingleCommand("grep -l -r '%s/%s\s%s' / 2> /dev/null" % (url_base,self.ofs_fs_name,self.ofs_mount_point),output)

        
        if rc == 0:
            print output
            self.setEnvironmentVariable("PVFS2TAB_FILE",output[1].rstrip())
        
        # to find source
        # find the directory
        #find / -name pvfs2-config.h.in -print 2> /dev/null
        # grep directory/configure 
        # grep -r 'prefix = /home/ec2-user/orangefs' /home/ec2-user/stable/Makefile
        

    def exportNFSDirectory(self,directory_name,options=None,network=None,netmask=None):
        if options == None:
            options = "rw,sync,no_root_squash,no_subtree_check"
        if network == None:
            network = self.ip_address
        if netmask == None:
            netmask = 24
        
        
        self.runSingleCommand("mkdir -p %s" % directory_name)
        commands = '''
        sudo bash -c 'echo "%s %s/%r(%s)" >> /etc/exports'
        #sudo service cups stop
        #sudo service sendmail stop
        #sudo service rpcbind restart
        #sudo service nfs restart
        sudo exportfs -a
        ''' % (directory_name,self.ip_address,netmask,options)
        
        self.runSingleCommandAsBatch(commands)
        time.sleep(30)
        
        return "%s:%s" % (self.ip_address,directory_name)
        
        
    def mountNFSDirectory(self,nfs_share,mountpoint,options=""):
        self.changeDirectory("/home/%s" % self.current_user)
        self.runSingleCommand("mkdir -p %s" % mountpoint)
        commands = 'sudo mount -t nfs -o %s %s %s' % (options,nfs_share,mountpoint)
        print commands
        self.runSingleCommandAsBatch(commands)
        output = []
        rc = self.runSingleCommand("mount -t nfs | grep %s" % nfs_share,output)
        count = 0
        while rc != 0 and count < 10 :
            time.sleep(15)
            self.runSingleCommandAsBatch(commands)
            rc = self.runSingleCommand("mount -t nfs | grep %s" % nfs_share,output)
            print output
            count = count + 1
        return 0
        
        
    
#===================================================================================================
# Unit test script begins here
#===================================================================================================
def test_driver():
    local_machine = OFSTestLocalNode()
    local_machine.addRemoteKey('10.20.102.54',"/home/jburton/buildbot.pem")
    local_machine.addRemoteKey('10.20.102.60',"/home/jburton/buildbot.pem")
    
    '''
    local_machine.changeDirectory("/tmp")
    local_machine.setEnvironmentVariable("FOO","BAR")
    local_machine.runSingleCommand("echo $FOO")
    local_machine.addBatchCommand("echo \"This is a test of the batch command system\"")
    local_machine.addBatchCommand("echo \"Current directory is `pwd`\"")
    local_machine.addBatchCommand("echo \"Variable foo is $FOO\"")
    local_machine.runAllBatchCommands()
    

    
    #local_machine.copyOFSSource("LOCALDIR","/home/jburton/testingjdb/","/tmp/jburton/testingjdb/")
    #local_machine.configureOFSSource()
    #local_machine.makeOFSSource()
    #local_machine.installOFSSource()
    '''
    
    remote_machine = OFSTestRemoteNode('ec2-user','10.20.102.54',"/home/jburton/buildbot.pem",local_machine)
    remote_machine1 = OFSTestRemoteNode('ec2-user','10.20.102.60', "/home/jburton/buildbot.pem",local_machine)

'''
    remote_machine.setEnvironmentVariable("LD_LIBRARY_PATH","/opt/db4/lib")
    remote_machine1.setEnvironmentVariable("LD_LIBRARY_PATH","/opt/db4/lib")

   # remote_machine.uploadNodeKeyFromLocal(local_machine)
   # remote_machine1.uploadNodeKeyFromLocal(local_machine)
    remote_machine.uploadRemoteKeyFromLocal(local_machine,remote_machine1.ip_address)
    remote_machine1.uploadRemoteKeyFromLocal(local_machine,remote_machine.ip_address)
    
    remote_machine.copyOFSSource("SVN","http://orangefs.org/svn/orangefs/trunk","/tmp/ec2-user/")
    print "Configuring remote source"
    remote_machine.configureOFSSource()
    remote_machine.makeOFSSource()
    remote_machine.installOFSSource()
    
    #remote_machine1.runSingleCommandAsBatch("sudo rm /tmp/mount/orangefs/touched")
    #remote_machine1.copyOFSSource("TAR","http://www.orangefs.org/downloads/LATEST/source/orangefs-2.8.7.tar.gz","/tmp/ec2-user/")


    print ""
    print "-------------------------------------------------------------------------"
    print "Configuring remote source without shared libraries on " + remote_machine.host_name
    print ""
    remote_machine.runSingleCommand("rm -rf /tmp/ec2-user")
    remote_machine.runSingleCommand("rm -rf /tmp/orangefs")
    remote_machine1.installBenchmarks("http://devorange.clemson.edu/pvfs/benchmarks-20121017.tar.gz","/tmp/ec2-user/benchmarks")
    remote_machine.copyOFSSource("SVN","http://orangefs.org/svn/orangefs/branches/stable","/tmp/ec2-user/")
    remote_machine.configureOFSSource()
    remote_machine.makeOFSSource()
    remote_machine.installOFSSource()

    remote_machine.configureOFSServer([remote_machine])
    remote_machine.stopOFSServer()
    remote_machine.startOFSServer()
    #remote_machine.stopOFSServer()
    print ""
    print "Checking to see if pvfs2 server is running..."
    remote_machine.runSingleCommand("ps aux | grep pvfs2")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs before mount..."
    remote_machine.runSingleCommand("ls -l /tmp/mount/orangefs")
    remote_machine.installKernelModule()
    remote_machine.startOFSClient()
    remote_machine.mountOFSFilesystem()
    print ""
    print "Checking to see if pvfs2 client is running..."
    remote_machine.runSingleCommand("ps aux | grep pvfs2")
    print ""
    print "Checking pvfs2 mount..."
    remote_machine.runSingleCommand("mount | (grep pvfs2 || echo \"Not Mounted\")")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs after mount..."
    remote_machine.runSingleCommand("ls -l /tmp/mount/orangefs")
    print ""
    print "Checking to see if mounted FS works..."
    remote_machine.runSingleCommandAsBatch("sudo touch /tmp/mount/orangefs/touched")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs after touch..."
    remote_machine.runSingleCommand("ls -l /tmp/mount/orangefs")
    print ""

    remote_machine.stopOFSClient()
    remote_machine.stopOFSServer()
    print "Checking to see if all pvfs2 services have stopped."
    remote_machine.runSingleCommand("ps aux | grep pvfs2")
    print ""

    print ""
    print "-------------------------------------------------------------------------"
    print "Configuring remote source with shared libraries on " + remote_machine1.host_name
    print ""
    remote_machine1.runSingleCommand("rm -rf /tmp/orangefs")
    remote_machine1.runSingleCommand("rm -rf /tmp/ec2-user")
    remote_machine1.installBenchmarks("http://devorange.clemson.edu/pvfs/benchmarks-20121017.tar.gz","/tmp/ec2-user/")
    remote_machine1.copyOFSSource("SVN","http://orangefs.org/svn/orangefs/branches/stable","/tmp/ec2-user/")


    remote_machine1.configureOFSSource("--enable-strict --enable-shared --enable-ucache --disable-karma --with-db=/opt/db4 --prefix=/tmp/orangefs --with-kernel=%s/build" % remote_machine1.getKernelVersion())
    #remote_machine1.configureOFSSource()
    remote_machine1.makeOFSSource()
    remote_machine1.installOFSSource()

    remote_machine1.configureOFSServer([remote_machine1])
    remote_machine1.stopOFSServer()
    remote_machine1.startOFSServer()
    #remote_machine1.stopOFSServer()
    print ""
    print "Checking to see if pvfs2 server is running..."
    remote_machine1.runSingleCommand("ps aux | grep pvfs2")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs before mount..."
    remote_machine1.runSingleCommand("ls -l /tmp/mount/orangefs")
    remote_machine1.installKernelModule()
    remote_machine1.startOFSClient()
    remote_machine1.mountOFSFilesystem()
    print ""
    print "Checking to see if pvfs2 client is running..."
    remote_machine1.runSingleCommand("ps aux | grep pvfs2")
    print ""
    print "Checking pvfs2 mount..."
    remote_machine1.runSingleCommand("mount | (grep pvfs2 || echo \"Not Mounted\")")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs after mount..."
    remote_machine1.runSingleCommand("ls -l /tmp/mount/orangefs")
    print ""
    print "Checking to see if mounted FS works"
    remote_machine1.runSingleCommandAsBatch("sudo touch /tmp/mount/orangefs/touched")
    print ""
    print "Checking to see what is in /tmp/mount/orangefs after touch..."
    remote_machine1.runSingleCommand("ls -l /tmp/mount/orangefs")

    remote_machine1.stopOFSClient()
    remote_machine1.stopOFSServer()
    print "Checking to see if all pvfs2 services have stopped..."
    remote_machine1.runSingleCommand("ps aux | grep pvfs2")
    print ""


    #export LD_LIBRARY_PATH=${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/lib:/opt/db4/lib
    #export PRELOAD="LD_PRELOAD=${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/lib/libofs.so:${PVFS2_DEST}/INSTALL-pvfs2-${CVS_TAG}/lib/libpvfs2.so

    #local_machine.copyToRemoteNode("/home/jburton/buildbot.pem",remote_machine,"~/buildbot.pem",False)
    #remote_machine.copyToRemoteNode("~/buildbot.pem",remote_machine1,"~/buildbot.pem",False)

    
    remote_machine.setEnvironmentVariable("FOO","BAR")
    remote_machine.runSingleCommand("echo $FOO")
    remote_machine.runSingleCommand("hostname -s")
    remote_machine.addBatchCommand("echo \"This is a test of the batch command system\"")
    remote_machine.addBatchCommand("echo \"Current directory is `pwd`\"")
    remote_machine.addBatchCommand("echo \"Variable foo is $FOO\"")
    remote_machine.addBatchCommand("touch /tmp/touched")
    remote_machine.addBatchCommand("sudo apt-get update && sudo apt-get -y dist-upgrade")
    remote_machine.addBatchCommand("sudo yum -y upgrade")
    remote_machine.runAllBatchCommands()
    
   ''' 
    
#Call script with -t to test
#if len(sys.argv) > 1 and sys.argv[1] == "-t":
#    test_driver()
