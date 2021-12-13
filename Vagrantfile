# -*- mode: ruby -*-
# vi: set ft=ruby :

require 'socket'

# Vagrantfile API/syntax version. Don't touch unless you know what you're doing!
VAGRANTFILE_API_VERSION = "2"

Vagrant.configure(VAGRANTFILE_API_VERSION) do |config|

  config.vm.box = "ubuntu/focal64"
  config.vm.hostname = "mapcache-vagrant"
  config.vm.network :forwarded_port, guest: 80, host: 8080
  config.vm.provider "virtualbox" do |v|
    v.customize ["modifyvm", :id, "--memory", 1024, "--cpus", 2]
    v.customize ["modifyvm", :id, "--ioapic", "on", "--largepages", "off", "--vtxvpid", "off"]
    v.name = "mapcache-vagrant"
  end

  config.vm.provision "shell", path: "scripts/vagrant/packages.sh"
  config.vm.provision "shell", path: "scripts/vagrant/mapcache.sh"

end
