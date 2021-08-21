MapCache
========

| |Build Status| |Appveyor Build Status|

-------
Summary
-------

MapCache is a server that implements tile caching to speed up access to WMS layers. The primary objectives are to be fast and easily deployable, 
while offering the essential features (and more!) expected from a tile caching solution.

For more  information and complete documentation please 
visit:

  http://mapserver.org/mapcache/
  
Questions relating to MapCache use and development can be asked on the MapServer mailing lists:

  http://www.mapserver.org/community/lists.html  
  
License
-------

::

	/******************************************************************************
	 *
	 * Project:  MapServer
	 * Purpose:  MapCache tile caching program.
	 * Author:   Thomas Bonfort and the MapServer team.
	 *
	 ******************************************************************************
	 * Copyright (c) 1996-2019 Regents of the University of Minnesota.
	 *
	 * Permission is hereby granted, free of charge, to any person obtaining a
	 * copy of this software and associated documentation files (the "Software"),
	 * to deal in the Software without restriction, including without limitation
	 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
	 * and/or sell copies of the Software, and to permit persons to whom the
	 * Software is furnished to do so, subject to the following conditions:
	 *
	 * The above copyright notice and this permission notice shall be included in
	 * all copies of this Software or works derived from this Software.
	 *
	 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
	 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
	 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
	 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	 * DEALINGS IN THE SOFTWARE.
	 *****************************************************************************/


.. |Build Status| image:: https://travis-ci.org/mapserver/mapcache.svg?branch=master
   :target: https://travis-ci.org/mapserver/mapcache

.. |Appveyor Build Status| image:: https://ci.appveyor.com/api/projects/status/7al5utxjh83ig71v?svg=true
   :target: https://ci.appveyor.com/project/mapserver/mapcache