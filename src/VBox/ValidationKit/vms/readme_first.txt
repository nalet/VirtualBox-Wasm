Rules for creating + deploying a smoketest VM
=============================================

# Rules for creating a smoketest VM

1. Make sure to double check the following parameters when creating the VM:
    - Which OS version / bitness (32- or 64-bit)?
    - BIOS or EFI?
    - Which storage controller (especially needed for Windows guests)?
2. No internet access must be required for a VM to run testcases as
   a prerequisite, e.g. no updating / installing new packages in testcases
   unless absolutely required.
3. Keep the VM as slim and barebone as possible, e.g. empty the trash and
   clean everything up before deploying the .vdi into the test resource tree.
4. Do not modify any guest OS system settings if possible, to have a vanilla
   guest OS experience.
5. Do NOT install any Guest Additions in the VM -- this will be part of our
   smoketests then.
6. Do NOT FORGET to deploy the Validation Kit tools and TestExecService (TXS) inside the VM
   and make sure that it works (use ValidationKit/utils/TestExecServ/vboxtxs-readme.txt as an instruction).
7. Use the following parameters for installation:
    * English (US) language + keyboard layout
    * Set timezone to Berlin (UTC+1)
    * User name: Administrator (Windows, see 7.1 step) / vbox (non-Windows)
    * User password: password
    * Create new user (needed for unattended installations):
      * User name: vboxuser
      * Password: changeme
    * Create new user
      * User name: test
      * Password: <no password>
    * Configure automatic log-in for user vbox
        - For Windows, run "netplwiz" and uncheck "Useres must enter [..]"
    * Disable / nod off first-run / welcome wizards
        - For Windows:
            * go to Computer Configuration->Administrative Templates->System->Logon
            * disable option Show first sign-in animation (win10)
            *
            or
            * use registry, HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System
            * set "EnableFirstLogonAnimation" to 0
    * Disable automatic updates
        - For Windows, do ```sc config wuauserv start= disabled```
    * Disable power management, if possible ("blank screen after X minutes ...")
    * Disable any screensavers
7.1 If vm is Windows you may be unable to create the "Administrator" user, so:
    * open "Computer Management" -> Local Users and Groups -> Users
    * rename built-in account "Administrator" (make it "Administrator2" for example)
    * rename your current user from "your-user-name" to "Administrator"
8. Post installation steps:
    * Eject any CD and shut down the VM.
    * Perform additional steps (e.g. for Linux VMs) as shown below.
    * Create a checksum file (SHA256) of the created .vdi named <name of .vdi>.sha256,
      for verification purposes.

# Rules for deploying a smoketest VM

1. Never delete and/or overwrite an existing test VM. Instead, alter the
   naming to versionize to tell things apart.

   Example for an altered VM:
     - Original test VM name "t-some-os.vdi"
     - New test VM name should be "t-some-os-<N+1>.vdi" -> "t-some-os-1.vdi"

2. Also ALWAYS upload the matching checksum file (<name of .vdi>.sha256).

## For Linux VMs

### Install Guest Additions dependencies:
    * Kernel headers
        - For Debian/Ubuntu: ```linux-headers-generic```
        - For OL/RHEL/CentOS: ```kernel-devel-$(uname -r)``` or ```kernel-uek-devel```
    * Other stuff
        - For Debian/Ubuntu: ```build-essential perl```
        - For OL/RHEL/CentOS: ```make automake gcc bzip2 perl [elfutils-libelf-devel]```
    * DO NOT install Guest Additions itself!

### Post install:

    * Make sure that the network interface comes up automatically
    * Disable any screensavers / lock screens, for getting meaningful screenshots
    * If applicable, disable SELinux and/or AppArmor:
        - /etc/selinux/config:
              SELINUX=disabled
          Note: A guest reboot is needed after disabling SELinux!
    * Cleanup:
        - Make sure that already distribution-provided Guest Addiitions
          have been uninstalled / blacklisted, e.g.
            ```sudo sh -c "echo 'blacklist vboxguest' >> /etc/modprobe.d/blacklist.conf"```
            ```sudo sh -c "echo 'blacklist vboxvideo' >> /etc/modprobe.d/blacklist.conf"```
        - For Debian/Ubuntu:
            a) via ```apt-get autoremove && apt-get autoclean```
            b) remove outdated kernels
        - For OL/RHEL/CentOS:
            a) ```yum clean all```

## For Windows VMs

Older Windows versions tend to nag quite a bit about activation. This also can become a
problem after some time, e.g. is not instantly visible when creating the test VM. So best
would be to find a sane (and legal!) way to either activate and/or turn off the activation
as a whole to not disturb automated testing.

## Useful to know

### Specifiying a proxy server

    On Debian/Ubuntu:
        Create a file /etc/apt/apt.conf.d/proxy.conf with:
            ```Acquire::http::Proxy "http://my-proxy:port";
            Acquire::https::Proxy "http://my-proxy:port";```

    On OL/RHEL/CentOS:
        Edit /etc/yum.conf by adding in [main]:
            ```proxy=https://my-proxy:port```

### Oracle Linux 6

    * The public yum repositories need to be installed first, via e.g.
        ```wget http://yum.oracle.com/public-yum-ol6.repo -O /etc/yum.repos.d/```
