# Building in VirtualBox using Vagrant and Ansible

If you want to quickly get Pyston up-and-running in a virtual machine,
you can use Vagrant to create a virtual machine in VirtualBox
running Ubuntu 14.04 (Trusty Tahr). The following steps
use Ansible to run the commands to download and compile Pyston
plus its dependencies.


## Requirements

You need to install:

- [Oracle VirtualBox](https://www.virtualbox.org/wiki/Downloads)
- [Vagrant](http://downloads.vagrantup.com/)
- [Ansible](http://docs.ansible.com/intro_installation.html#installing-the-control-machine) - If you're on a Mac and you use [homebrew](http://brew.sh), you can just run "brew install ansible"

## Building

From a checkout directory of Pyston, enter
the `tools/vagrant` directory, and enter:

```
$ vagrant up
```

And then wait a while, probably an hour or two.

## Using the Virtual Machine

Once the machine has finished installing (vagrant calls this provisioning),
you can connect to it with:

```
$ vagrant ssh
```

Then once you're in the virtual machine:

```
$ cd pyston/src
$ ./pyston_dbg
```



