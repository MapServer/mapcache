Unix compilation instructions
-----------------------------

Mapcache now builds with cmake. It seems much happier if it can find apxs, which means you
might need apache2-prefork installed (on ubuntu apt-get install apache2-prefork-dev).

```
  cd mapcache
  mkdir build
  cd build
  cmake ..
  make
  sudo make install
```

Detailed instructions and configuration options are maintained in the mapcache documentation : 
http://mapserver.org/mapcache/install.html
