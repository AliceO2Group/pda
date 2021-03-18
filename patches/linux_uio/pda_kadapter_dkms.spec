%define module pda-kadapter-dkms
%define version 1.2.0

Summary: PDA kernel adapter DKMS package
Name: %{module}
Version: %{version}
Release: 0
License: BSD
Packager: Pascal Boeschoten
Group: System Environment/Base
BuildArch: noarch
Requires: dkms >= 1.00, kernel-devel, kernel-headers
Requires: bash
Source0: %{module}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root/

%description
This package contains the PDA kernel adapter wrapped for the DKMS framework.

%prep
#rm -rf %{module}-%{version}
#mkdir %{module}-%{version}
#cd %{module}-%{version}


%install
if [ "$RPM_BUILD_ROOT" != "/" ]; then
	rm -rf $RPM_BUILD_ROOT
fi
mkdir -p $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/
tar xvzf $RPM_SOURCE_DIR/%{module}-%{version}.tar.gz --strip-components=1 -C $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/

mkdir -p $RPM_BUILD_ROOT/etc/udev/rules.d/
mv $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/99-pda.rules $RPM_BUILD_ROOT/etc/udev/rules.d/%{module}-%{version}.rules

mv $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/Makefile_dkms $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/Makefile

%clean
#if [ "$RPM_BUILD_ROOT" != "/" ]; then
#	rm -rf $RPM_BUILD_ROOT
#fi

%files
%defattr(644,root,root,755)
/usr/src/%{module}-%{version}/
/etc/udev/rules.d/%{module}-%{version}.rules

%pre

%post
dkms add -m %{module} -v %{version}

if [ `uname -r | grep -c "BOOT"` -eq 0 ]; then
	dkms build -m %{module} -v %{version}
	dkms install -m %{module} -v %{version}
elif [ `uname -r | grep -c "BOOT"` -gt 0 ]; then
	echo -e ""
	echo -e "Module build for the currently running kernel was skipped since you"
	echo -e "are running a BOOT variant of the kernel."
else
	echo -e ""
	echo -e "Module build for the currently running kernel was skipped since the"
	echo -e "kernel headers for this kernel do not seem to be installed."
fi
exit 0

%preun
echo -e
echo -e "Uninstall of PDA kernel adapter module (version %{version}) beginning:"
dkms remove -m %{module} -v %{version} --all
exit 0

