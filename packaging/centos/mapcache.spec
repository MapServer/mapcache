Name:           mapcache
Version:        0.1.0
Release:        0.rc1%{?dist}
Summary:        Caching server for WMS layers
Group:          Development/Tools
License:        MIT
URL:            http://www.mapserver.org/trunk/en/mapcache/
Source:         https://github.com/mapserver/mapcache/mapcache-0.1.0-rc1.tar.gz
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires:       httpd

BuildRequires:  httpd-devel fcgi-devel
BuildRequires:  geos-devel proj-devel gdal-devel cairo-devel
BuildRequires:  libjpeg-devel libpng-devel fribidi-devel giflib-devel curl-devel libtiff-devel
BuildRequires:  sqlite-devel


%description
MapCache is a server that implements tile caching to speed up access to WMS layers. 
The primary objectives are to be fast and easily deployable, while offering the 
essential features (and more!) expected from a tile caching solution.

%prep
%setup -q -n mapcache-0.1.0-rc1

%build
CFLAGS="${CFLAGS} -ldl" ; export CFLAGS

%configure \
	--prefix=%{_prefix} 

make

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} \
	install

# apache module
cd apache
make DESTDIR=%{buildroot} \
	install


#mkdir -p %{buildroot}/%{_libexecdir}
#mv %{buildroot}/%{_bindir}/mapcache %{buildroot}/%{_libexecdir}/


%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root)
%doc INSTALL README* LICENSE 
%{_bindir}/*
%{_libdir}/*

%changelog
