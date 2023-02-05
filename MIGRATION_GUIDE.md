Migrating from MapCache 1.12 to 1.14
====================================

* This is a security release, to handle a string formatting injection vulnerability, 
  and includes other changes.
  No backward compatibility issue is expected.
  See [MapCache 1.14 Changelog](https://mapserver.org/development/changelog/mapcache/changelog-1-14.html).

Migrating from MapCache 1.10 to 1.12
====================================

* No backward compatibility issue is expected.
  See [MapCache 1.12 Changelog](https://mapserver.org/development/changelog/mapcache/changelog-1-12.html)
  for a list of bug fixes and new features.

Migrating from MapCache 1.8 to 1.10
===================================

* No backward compatibility issue is expected.
  See [MapCache 1.10 Changelog](https://mapserver.org/development/changelog/mapcache/changelog-1-10.html)
  for a list of bug fixes and new features.

Migrating from MapCache 1.6 to 1.8
==================================

* <dimensions type="time" ...>...<query>SQL</query> should be replaced by
  <dimensions type="time" ...>...<validate_query>SQL</validate_query><list_query>...</list_query> or
  <dimensions type="sqlite" time="true" ...>...<validate_query>SQL</validate_query><list_query>...</list_query>
  (see [RFC-121](https://mapserver.org/development/rfc/ms-rfc-121.html) for full examples)

Migrating from MapCache 1.4 to 1.6
==================================

* The <timedimension> tileset child has been removed. Time dimensions are now added with <dimension type="time">

* <dimension type="values" ...>val1,val2,val3</dimension> should be replaced by
  <dimension type="values"><value>val1</value><value>val2</value><value>val3</value></dimension>

* <dimension type="values" case_sensitive="true">...</dimension> should be replaced by
  <dimension type="values"><case_sensitive>true</case_sensitive>....</dimension>

* <dimension type="regex" ...>^abc$</dimension> should be replaced by
  <dimension type="regex"><regex>^abc$</regex></dimension>

* <dimension ... assembly="stack">...</dimension> should be replaced by
  <dimensions><assembly_type>stack</assembly_type><dimension ...>....</dimension></dimensions>
