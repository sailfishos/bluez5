Name:       bluez5

Summary:    Bluetooth daemon
Version:    5.58
Release:    1
License:    GPLv2+
URL:        http://www.bluez.org/
Source0:    http://www.kernel.org/pub/linux/bluetooth/%{name}-%{version}.tar.gz
Source1:    obexd-wrapper
Source2:    obexd.conf
Source3:    bluez.tracing
Source4:    obexd.tracing
Requires:   %{name}-libs = %{version}-%{release}
Requires:   dbus >= 0.60
Requires:   hwdata >= 0.215
Requires:   bluez5-configs
Requires:   systemd
Requires:   oneshot
# /etc/obexd.conf requires find
Requires:   findutils
# For bluetooth group
Requires:   sailfish-setup
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libusb)
BuildRequires:  pkgconfig(udev)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(check)
BuildRequires:  pkgconfig(libical)
BuildRequires:  bison
BuildRequires:  flex
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
Conflicts: bluez

%description
%{summary}.

%package configs-mer
Summary:    Bluetooth (bluez5) default configuration
Requires:   %{name} = %{version}-%{release}
Provides:   bluez5-configs
Conflicts:  bluez-configs-mer
%description configs-mer
%{summary}.

%package cups
Summary:    Bluetooth (bluez5) CUPS support
Requires:   %{name} = %{version}-%{release}
Requires:   cups
Conflicts:  bluez-cups
%description cups
%{summary}.

%package doc
Summary:    Bluetooth (bluez5) daemon documentation
Requires:   %{name} = %{version}-%{release}
Conflicts:  bluez-doc
%description doc
%{summary}.

%package hcidump
Summary:    Bluetooth (bluez5) packet analyzer
Requires:   %{name} = %{version}-%{release}
Conflicts:  bluez-hcidump
%description hcidump
%{summary}.

%package libs
Summary:    Bluetooth (bluez5) library
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
Conflicts:  bluez-libs
%description libs
%{summary}.

%package libs-devel
Summary:    Bluetooth (bluez5) library development package
Requires:   bluez5-libs = %{version}
Conflicts:  bluez-libs-devel
%description libs-devel
%{summary}.

%package test
Summary:    Test utilities for Bluetooth (bluez5)
Requires:   %{name} = %{version}-%{release}
Requires:   %{name}-libs = %{version}
Requires:   dbus-python
Requires:   pygobject2 >= 3.10.2
Conflicts:  bluez-test
%description test
%{summary}.

%package tools
Summary:    Command line tools for Bluetooth (bluez5)
# Readline is GPLv3+
BuildRequires: pkgconfig(readline)
Requires:   %{name} = %{version}-%{release}
Requires:   %{name}-tools-hciattach = %{version}-%{release}
Conflicts:  bluez-tools
%description tools
%{summary}.

%package tools-hciattach
Summary:    Command line tool for Bluetooth (bluez5)
Requires:   %{name} = %{version}-%{release}
Conflicts:  bluez-tools
%description tools-hciattach
%{summary}.

%package obexd
Summary:    OBEX server (bluez5)
Requires:   %{name} = %{version}-%{release}
Requires:   obex-capability
Conflicts:  obexd
Conflicts:  obexd-server
%description obexd
%{summary}.

%package obexd-tools
Summary:    Command line tools for OBEX (bluez5)
%description obexd-tools
%{summary}.

%package tracing
Summary:    Configuration for bluez5 to enable tracing
Requires:   %{name} = %{version}-%{release}
Conflicts:  bluez-tracing
%description tracing
Will enable tracing for BlueZ 5

%package obexd-tracing
Summary:    Configuration for bluez5-obexd to enable tracing
%description obexd-tracing
Will enable tracing for BlueZ 5 OBEX daemon

%prep
%autosetup -n %{name}-%{version}

./bootstrap

%build
autoreconf --force --install

%configure \
    --with-contentfilter=helperapp \
    --with-phonebook=sailfish \
    --with-systemdsystemunitdir=%{_unitdir} \
    --with-systemduserunitdir=%{_userunitdir} \
    --enable-deprecated \
    --enable-hid2hci \
    --enable-jolla-blacklist \
    --enable-jolla-dbus-access \
    --enable-jolla-did \
    --enable-library \
    --enable-option-checking \
    --enable-sailfish-exclude \
    --enable-sixaxis \
    --enable-test \
    --disable-autopair \
    --disable-hostname

%make_build

%check

%install
rm -rf %{buildroot}
%make_install

# bluez systemd integration
mkdir -p $RPM_BUILD_ROOT/%{_unitdir}/network.target.wants
ln -s ../bluetooth.service $RPM_BUILD_ROOT/%{_unitdir}/network.target.wants/bluetooth.service
(cd $RPM_BUILD_ROOT/%{_unitdir} && ln -s bluetooth.service dbus-org.bluez.service)

# bluez runtime files
install -d -m 0755 $RPM_BUILD_ROOT/%{_localstatedir}/lib/bluetooth

# bluez configuration
mkdir -p ${RPM_BUILD_ROOT}%{_sysconfdir}/bluetooth
for CONFFILE in profiles/input/input.conf profiles/network/network.conf src/main.conf ; do
install -v -m644 ${CONFFILE} ${RPM_BUILD_ROOT}%{_sysconfdir}/bluetooth/`basename ${CONFFILE}`
done

mkdir -p %{buildroot}%{_sysconfdir}/tracing/bluez/
cp -a %{SOURCE3} %{buildroot}%{_sysconfdir}/tracing/bluez/

# obexd systemd/D-Bus integration
(cd $RPM_BUILD_ROOT/%{_userunitdir} && ln -s obex.service dbus-org.bluez.obex.service)

# obexd wrapper
install -m755 -D %{SOURCE1} ${RPM_BUILD_ROOT}/%{_libexecdir}/obexd-wrapper
install -m644 -D %{SOURCE2} ${RPM_BUILD_ROOT}/%{_sysconfdir}/obexd.conf
sed -i 's,Exec=.*,Exec=/usr/libexec/obexd-wrapper,' \
    ${RPM_BUILD_ROOT}/%{_datadir}/dbus-1/services/org.bluez.obex.service
sed -i 's,ExecStart=.*,ExecStart=/usr/libexec/obexd-wrapper,' \
${RPM_BUILD_ROOT}/%{_userunitdir}/obex.service

# obexd configuration
mkdir -p ${RPM_BUILD_ROOT}/%{_sysconfdir}/obexd/{plugins,noplugins}

# HACK!! copy manually missing tools
cp -a tools/bluetooth-player %{buildroot}%{_bindir}/
cp -a tools/btmgmt %{buildroot}%{_bindir}/
cp -a attrib/gatttool %{buildroot}%{_bindir}/
cp -a tools/obex-client-tool %{buildroot}%{_bindir}/
cp -a tools/obex-server-tool %{buildroot}%{_bindir}/
cp -a tools/obexctl %{buildroot}%{_bindir}/

# HACK!! copy manually missing test scripts
cp -a test/exchange-business-cards %{buildroot}%{_libdir}/bluez/test/
cp -a test/get-managed-objects %{buildroot}%{_libdir}/bluez/test/
cp -a test/get-obex-capabilities %{buildroot}%{_libdir}/bluez/test/
cp -a test/list-folders %{buildroot}%{_libdir}/bluez/test/
cp -a test/simple-obex-agent %{buildroot}%{_libdir}/bluez/test/

mkdir -p %{buildroot}%{_sysconfdir}/tracing/obexd/
cp -a %{SOURCE4} %{buildroot}%{_sysconfdir}/tracing/obexd/

# Rename pkg-config file to differentiate from BlueZ 4.x
mv %{buildroot}%{_libdir}/pkgconfig/bluez.pc %{buildroot}%{_libdir}/pkgconfig/bluez5.pc

# We don't need zsh stuff
rm -rf %{buildroot}%{_datadir}/zsh

# there is no macro for /lib/udev afaict
%define udevlibdir /lib/udev

%post libs -p /sbin/ldconfig

%postun libs -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libexecdir}/bluetooth/bluetoothd
%{_libdir}/bluetooth/plugins/sixaxis.so
%{_datadir}/dbus-1/system-services/org.bluez.service
/%{_unitdir}/bluetooth.service
/%{_unitdir}/network.target.wants/bluetooth.service
/%{_unitdir}/dbus-org.bluez.service
%config %{_sysconfdir}/dbus-1/system.d/bluetooth.conf
%dir %{_localstatedir}/lib/bluetooth

%files configs-mer
%defattr(-,root,root,-)
%config %{_sysconfdir}/bluetooth/*

%files cups
%defattr(-,root,root,-)
%{_libdir}/cups/backend/bluetooth

%files doc
%defattr(-,root,root,-)
%doc %{_mandir}/man1/*.1.gz
%doc %{_mandir}/man8/*.8.gz

%files hcidump
%defattr(-,root,root,-)
%{_bindir}/hcidump

%files libs
%defattr(-,root,root,-)
%license COPYING
%{_libdir}/libbluetooth.so.*

%files libs-devel
%defattr(-,root,root,-)
%{_libdir}/libbluetooth.so
%dir %{_includedir}/bluetooth
%{_includedir}/bluetooth/*
%{_libdir}/pkgconfig/bluez5.pc

%files test
%defattr(-,root,root,-)
%{_libdir}/bluez/test/*

%files tools
%defattr(-,root,root,-)
%{_bindir}/bluetooth-player
%{_bindir}/bluemoon
%{_bindir}/bluetoothctl
%{_bindir}/btattach
%{_bindir}/btmgmt
%{_bindir}/btmon
%{_bindir}/ciptool
%{_bindir}/gatttool
%{_bindir}/hciconfig
%{_bindir}/hcitool
%{_bindir}/hex2hcd
%{_bindir}/l2ping
%{_bindir}/l2test
%{_bindir}/mpris-proxy
%{_bindir}/rctest
%{_bindir}/rfcomm
%{_bindir}/sdptool
/%{udevlibdir}/hid2hci
/%{_udevrulesdir}/97-hid2hci.rules

%files tools-hciattach
%defattr(-,root,root,-)
%{_bindir}/hciattach

%files obexd
%defattr(-,root,root,-)
%config %{_sysconfdir}/obexd.conf
%dir %{_sysconfdir}/obexd/
%dir %{_sysconfdir}/obexd/plugins/
%dir %{_sysconfdir}/obexd/noplugins/
%attr(2755,root,privileged) %{_libexecdir}/bluetooth/obexd
%{_libexecdir}/obexd-wrapper
%{_datadir}/dbus-1/services/org.bluez.obex.service
%{_userunitdir}/obex.service
%{_userunitdir}/dbus-org.bluez.obex.service

%files obexd-tools
%defattr(-,root,root,-)
%{_bindir}/obex-client-tool
%{_bindir}/obex-server-tool
%{_bindir}/obexctl

%files tracing
%defattr(-,root,root,-)
%dir %{_sysconfdir}/tracing/bluez
%config %{_sysconfdir}/tracing/bluez/bluez.tracing

%files obexd-tracing
%defattr(-,root,root,-)
%dir %{_sysconfdir}/tracing/obexd
%config %{_sysconfdir}/tracing/obexd/obexd.tracing
