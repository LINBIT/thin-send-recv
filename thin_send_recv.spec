Name: thin-send-recv
Version: 1.0
Release: 1
Summary: send and receive for LVM thin volumes
License: GPLv3+
Group: Applications/System

Requires: /usr/sbin/lvs
Requires: /usr/sbin/thin_dump
Requires: /usr/sbin/thin_delta
Requires: /bin/sh

Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-root
BuildRequires: flex make gcc

%description
thin_send serializes a thin volume into a stream. It is more efficient than
dd by only sending allocated data blocks. (In contrast a stream produced by
dd contains lots of zeroes for all not allocated blocks). thin_recv decodes
that stream and applies it to a thin volume.
Thins_send can also create a stream that contains only the blocks modified
by a snapshot or between two snapshots.

%prep
%setup -q -n %{name}-%{version}

%build
make EXTRA_CFLAGS=-g

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%doc README.md
%{_bindir}/*

%changelog
* Tue Nov 23 2021 Philipp Reisner <philipp.reisner@linbit.com> - 1.0-1
- activation flags, signals, tests

* Thu Sep 10 2020 Robert Altnoeder <robert.altnoeder@linbit.com> - 0.24-1
- Workaround for external LVM commands race condition

* Wed Aug 05 2020 Roland Kammerer <roland.kammerer@linbit.com> - 0.23-1
- New upstream release

* Fri Jul 24 2020 Philipp Reisner <philipp.reisner@linbit.com> - 0.22-1
- Splice always.

* Tue Dec 17 2019 Philipp Reisner <philipp.reisner@linbit.com> - 0.11-1
- Initial build.

