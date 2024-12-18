#!/bin/sh

# script to build pda-kadapter-dkms RPM from upstream repo
# as used in O2 FLP farm
# sylvain.chapeland@cern.ch


# This is pda GIT tag from https://github.com/cbm-fles/pda
GIT_TAG=11.8.7

# This is the base name and version for this dkms package
PKG_NAME=pda-kadapter-dkms
PKG_VERSION=2.1.3

# local build directory
TMPDIR=/tmp/rpm

# current directory
CURDIR=`pwd`

# use local versions of kernel module source files, if found
# (eg to test a local version not available in upstream repo)
USE_LOCAL_SOURCES=0

# Prerequisites:
# yum install -y git rpm-build

# check needed commands found
ISERROR=0
for a in git rpmbuild; do
  which $a > /dev/null 2>&1
  if [ "$?" -ne "0" ]; then
    echo "Missing: $a"
    ISERROR=1
  fi
done
if [ "$ISERROR" -ne "0" ]; then
  exit 0
fi

# create a fresh build directory tree
echo "Generating RPM in ${TMPDIR}"
rm -rf ${TMPDIR}
WDIR=${TMPDIR}/tmp
mkdir -p ${TMPDIR}/SOURCES ${TMPDIR}/SPECS ${TMPDIR}/BUILD ${TMPDIR}/RPMS ${TMPDIR}/SRPMS ${WDIR}

# get source code from upstream
echo "Generating source tarball ${TMPDIR}/SOURCES/${PKG_DIR}.src.tar.gz"
cd ${WDIR}
git clone https://github.com/cbm-fles/pda.git -c advice.detachedHead=false
cd pda
git fetch
#git checkout -b fix_sysfs_attr origin/fix_sysfs_attr
git checkout tags/${GIT_TAG}
cd patches/linux_uio

# update source code with local files, if present
if [ "$USE_LOCAL_SOURCES" == "1" ]; then
  for ff in uio_pci_dma.c uio_pci_dma.h; do
    if [ -f ${CURDIR}/${ff} ]; then
      echo "*** Using local file: ${ff}"
      cp -p ${CURDIR}/${ff} .
    fi
  done
fi

# create source tarball
KMOD_VERSION=`cat uio_pci_dma.h | grep UIO_PCI_DMA_VERSION | cut -d\" -f2`
echo "Detected uio_pci_dma  kernel module version ${KMOD_VERSION}"
VERSION="${PKG_VERSION}.${KMOD_VERSION}"
PKG_DIR=${PKG_NAME}-${VERSION}
mkdir -p ${WDIR}/${PKG_DIR}
cp -p Makefile.dkms dkms.conf uio_pci_dma.c uio_pci_dma.h *-pda.rules *-pda.conf ${WDIR}/${PKG_DIR}
cp ${WDIR}/pda/LICENSE ${WDIR}/${PKG_DIR}
cd ${WDIR}
tar -cf ${PKG_DIR}.src.tar ${PKG_DIR}
gzip ${PKG_DIR}.src.tar
mv ${PKG_DIR}.src.tar.gz ${TMPDIR}/SOURCES

# create specfile
SPECFILE=${PKG_NAME}.spec
echo "Generating ${TMPDIR}/SPECS/${SPECFILE}"
rm -f ${TMPDIR}/SPECS/${SPECFILE}

# NB: https://en.opensuse.org/openSUSE:Package_group_guidelines
# Development/Sources is intended for binary and noarch packages containing sources. It is the right place for kernel sources and kernel module sources.
# System/Kernel contains kernel binaries and kernel-related tools like module-init-tools. The packages with kernel sources and kernel modules sources are in the group Development/Sources.
# For RHEL9, using : System Environment/Kernel
# rpm -qa --qf "%{GROUP}\n" | sort | uniq

echo "%define version ${VERSION}" >> ${TMPDIR}/SPECS/${SPECFILE}
echo "%define module ${PKG_NAME}" >> ${TMPDIR}/SPECS/${SPECFILE}

echo '
Summary: PDA kernel adapter DKMS package
Name: %{module}
Version: %{version}
Release: 0
License: BSD
URL: https://github.com/cbm-fles/pda/
Packager: Sylvain Chapeland <sylvain.chapeland@cern.ch>
Group: System Environment/Kernel
BuildArch: noarch
Requires: dkms >= 1.00, kernel-devel, kernel-headers
Requires: bash
Source0: %{module}-%{version}.src.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root/

%description
This package contains the PDA kernel adapter wrapped for the DKMS framework.

%prep
%setup

%install
mkdir -p $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/
mkdir -p $RPM_BUILD_ROOT/usr/lib/udev/rules.d/
mkdir -p $RPM_BUILD_ROOT/usr/lib/modules-load.d/
mv $RPM_BUILD_DIR/%{module}-%{version}/*-pda.rules $RPM_BUILD_ROOT/usr/lib/udev/rules.d/
mv $RPM_BUILD_DIR/%{module}-%{version}/*-pda.conf $RPM_BUILD_ROOT/usr/lib/modules-load.d/
mv $RPM_BUILD_DIR/%{module}-%{version}/Makefile.dkms $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/Makefile
mv $RPM_BUILD_DIR/%{module}-%{version}/* $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/


%clean
rm -rf $RPM_BUILD_DIR/%{module}-%{version}
rm -rf $RPM_BUILD_ROOT


%files
%defattr(644,root,root,755)
/usr/src/%{module}-%{version}/
/usr/lib/udev/rules.d/*.rules
/usr/lib/modules-load.d/*.conf


%pre
# ensure group pda exists
groupadd -f pda

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
' >> ${TMPDIR}/SPECS/${SPECFILE}

rpmbuild --define "_topdir ${TMPDIR}" -ba ${TMPDIR}/SPECS/${SPECFILE}