# -*- mode: ruby -*-
# vi: set ft=ruby :

git_user_name = `git config --get user.name`.strip
git_user_email = `git config --get user.email`.strip

Vagrant.configure(2) do |config|

  config.vm.box = "ubuntu/trusty64"
  config.vm.box_check_update = false
  config.vm.synced_folder ".", "/home/vagrant/pyston"
  config.vm.provider "virtualbox" do |vb|
    vb.memory = "2048"
    vb.cpus = 4
  end

  config.vm.provision "shell", inline: <<-SHELL
    sudo apt-get update
    sudo apt-get install -yq git
    sudo apt-get install -yq cmake
    sudo apt-get install -yq ninja-build
    sudo apt-get install -yq ccache
    sudo apt-get install -yq libncurses5-dev
    sudo apt-get install -yq liblzma-dev
    sudo apt-get install -yq libreadline-dev
    sudo apt-get install -yq libgmp3-dev
    sudo apt-get install -yq libmpfr-dev
    sudo apt-get install -yq autoconf
    sudo apt-get install -yq libtool
    sudo apt-get install -yq python-dev
    sudo apt-get install -yq texlive-extra-utils
    sudo apt-get install -yq clang
    sudo apt-get install -yq libssl-dev
    sudo apt-get install -yq libsqlite3-dev
    sudo apt-get install -yq pkg-config
    sudo apt-get install -yq libbz2-dev
    sudo apt-get install -yq gdb
    sudo apt-get install -yq zsh
  SHELL

  config.vm.provision "shell", privileged: false, inline: <<-SHELL
    echo "export LC_ALL=en_US.UTF-8" >> ~/.bashrc
    echo "export LANG=en_US.UTF-8" >> ~/.bashrc
    git config --global user.email "#{git_user_email}"
    git config --global user.name "#{git_user_name}"
    git clone https://github.com/llvm-mirror/llvm.git ~/pyston_deps/llvm-trunk
    git clone https://github.com/llvm-mirror/clang.git ~/pyston_deps/llvm-trunk/tools/clang
    cd ~/pyston && git submodule update --init --recursive build_deps
    cd ~/pyston && make llvm_up
  SHELL
end
