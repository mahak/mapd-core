#!/bin/bash

set -e
set -x

# Parse inputs
TSAN=false
COMPRESS=false
SAVE_SPACE=false
UPDATE_PACKAGES=false
CACHE=

# Establish number of cores to compile with
# Default to 8, Limit to 24
# Can be overridden with --nproc option
NPROC=$(nproc)
NPROC=${NPROC:-8}
if [ "${NPROC}" -gt "24" ]; then
  NPROC=24
fi

while (( $# )); do
  case "$1" in
    --update-packages)
      UPDATE_PACKAGES=true
      ;;
    --compress)
      COMPRESS=true
      ;;
    --savespace)
      SAVE_SPACE=true
      ;;
    --tsan)
      TSAN=true
      ;;
    --cache=*)
      CACHE="${1#*=}"
      ;;
    --static)
      ;;
    --shared)
      echo "ERROR - Only --static mode supported for Rocky"
      exit
      ;;
    --nproc=*)
      NPROC="${1#*=}"
      ;;
    *)
      break
      ;;
  esac
  shift
done

# Establish architecture
ARCH=$(uname -m)

if [ "$ARCH" != "x86_64" ] ; then
  echo "ERROR - Rocky only supported on x86_64"
  exit
fi

if [[ -n $CACHE && ( ! -d $CACHE  ||  ! -w $CACHE )  ]]; then
  # To prevent possible mistakes CACHE must be a writable directory
  echo "Invalid cache argument [$CACHE] supplied. Ignoring."
  CACHE=
fi

echo "Building with ${NPROC} cores"

if [[ ! -x  "$(command -v sudo)" ]] ; then
  if [ "$EUID" -eq 0 ] ; then
    dnf install -y sudo  
  else
    echo "ERROR - sudo not installed and not running as root"
    exit
  fi
fi

SUFFIX=${SUFFIX:=$(date +%Y%m%d)}
PREFIX=${MAPD_PATH:="/usr/local/mapd-deps/$SUFFIX"}

if [ ! -w $(dirname $PREFIX) ] ; then
    SUDO=sudo
fi
$SUDO mkdir -p $PREFIX
$SUDO chown -R $(id -u) $PREFIX
export PATH=$PREFIX/bin:$PATH
export LD_LIBRARY_PATH=$PREFIX/lib64:$PREFIX/lib:$LD_LIBRARY_PATH

# Needed to find xmltooling and xml_security_c
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PKG_CONFIG_PATH

SCRIPTS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source $SCRIPTS_DIR/common-functions-rockylinux.sh

sudo dnf groupinstall -y "Development Tools"

install_required_rockylinux_packages

generate_deps_version_file

# mold fast linker
install_mold_precompiled_x86_64

# gmp, mpc, mpfr, autoconf, automake
# note: if gmp fails on POWER8:
# wget https://gmplib.org/repo/gmp/raw-rev/4a6d258b467f
# patch -p1 < 4a6d258b467f
# https://gmplib.org/download/gmp/gmp-6.1.2.tar.xz
download_make_install ${HTTP_DEPS}/gmp-6.1.2.tar.xz "" "--enable-fat"

# http://www.mpfr.org/mpfr-current/mpfr-3.1.5.tar.xz
download_make_install ${HTTP_DEPS}/mpfr-4.0.1.tar.xz "" "--with-gmp=$PREFIX"
download_make_install ftp://ftp.gnu.org/gnu/mpc/mpc-1.1.0.tar.gz "" "--with-gmp=$PREFIX"
download_make_install ftp://ftp.gnu.org/gnu/autoconf/autoconf-2.69.tar.xz # "" "--build=powerpc64le-unknown-linux-gnu"  
download_make_install ftp://ftp.gnu.org/gnu/automake/automake-1.16.1.tar.xz 

install_gcc

export CC=$PREFIX/bin/gcc
export CXX=$PREFIX/bin/g++

install_ninja

install_maven

install_openssl

install_openldap2

# install_idn2
VERS=2.2.0
CFLAGS="-fPIC" download_make_install ${HTTP_DEPS}/libidn2-$VERS.tar.gz "" "--disable-shared --enable-static"

install_unistring

download_make_install ftp://ftp.gnu.org/gnu/libtool/libtool-2.4.6.tar.gz

# requires libtool
install_libmd

# TODO: move xml2 after readline
install_xml2

install_cmake

# icu and bzip2 (needed for boost)
install_icu

install_bzip2

install_boost
# TODO(scb): BOOST_ROOT may no longer be necessary (it was only to build xerces-c *I think*)
export BOOST_ROOT=$PREFIX/include

# http://zlib.net/zlib-1.2.8.tar.xz
download_make_install ${HTTP_DEPS}/zlib-1.2.8.tar.xz

install_uriparser

# libarchive
CFLAGS="-fPIC" download_make_install ${HTTP_DEPS}/xz-5.2.4.tar.xz "" "--disable-shared --with-pic"
CFLAGS="-fPIC" download_make_install ${HTTP_DEPS}/libarchive-3.3.2.tar.gz "" "--without-openssl --disable-shared" 

CFLAGS="-fPIC" download_make_install ftp://ftp.gnu.org/pub/gnu/ncurses/ncurses-6.4.tar.gz # "" "--build=powerpc64le-unknown-linux-gnu" 

download_make_install ftp://ftp.gnu.org/gnu/bison/bison-3.4.2.tar.xz # "" "--build=powerpc64le-unknown-linux-gnu" 

# https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/flexpp-bisonpp/bisonpp-1.21-45.tar.gz
download_make_install ${HTTP_DEPS}/bisonpp-1.21-45.tar.gz bison++-1.21

CFLAGS="-fPIC" download_make_install ftp://ftp.gnu.org/gnu/readline/readline-7.0.tar.gz "" "--disable-shared"

install_double_conversion

install_archive

VERS=0.3.5
CXXFLAGS="-fPIC -std=c++11" download_make_install https://github.com/google/glog/archive/v$VERS.tar.gz glog-$VERS "--enable-shared=no" # --build=powerpc64le-unknown-linux-gnu"

VERS=8.9.1
# https://curl.haxx.se/download/curl-$VERS.tar.xz
download_make_install ${HTTP_DEPS}/curl-$VERS.tar.xz "" "--disable-ldap --disable-ldaps --with-openssl"

# cpr
install_cpr

# thrift
install_thrift

# librdkafka
install_rdkafka

# backend rendering
VERS=1.6.21
# http://download.sourceforge.net/libpng/libpng-$VERS.tar.xz
download_make_install ${HTTP_DEPS}/libpng-$VERS.tar.xz

install_snappy
 
VERS=3.52.16
CFLAGS="-fPIC" CXXFLAGS="-fPIC" download_make_install ${HTTP_DEPS}/libiodbc-${VERS}.tar.gz

# c-blosc
install_blosc

# zstd required by GDAL and Arrow
install_zstd

# Geo Support
install_gdal_and_pdal
install_gdal_tools
install_geos

# http://thrysoee.dk/editline/libedit-20230828-3.1.tar.gz
CPPFLAGS="-I$PREFIX/include/ncurses" download_make_install ${HTTP_DEPS}/libedit-20230828-3.1.tar.gz

# llvm
install_llvm 

install_iwyu 

# TBB
install_tbb

# OneDAL
install_onedal

# Go
install_go

# install AWS core and s3 sdk
install_awscpp

# LZ4 required by rdkafka and Arrow
install_lz4

# Apache Arrow
install_arrow

# abseil
install_abseil

# glslang (with spirv-tools)
VERS=11.6.0 # stable 8/25/21
rm -rf glslang
mkdir -p glslang
pushd glslang
wget --continue https://github.com/KhronosGroup/glslang/archive/$VERS.tar.gz
tar xvf $VERS.tar.gz
pushd glslang-$VERS
./update_glslang_sources.py
mkdir build
pushd build
cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=$PREFIX \
    ..
make -j $(nproc)
make install
popd # build
popd # glslang-$VERS
popd # glslang

# spirv-cross
VERS=2020-06-29 # latest from 6/29/20
rm -rf spirv-cross
mkdir -p spirv-cross
pushd spirv-cross
wget --continue https://github.com/KhronosGroup/SPIRV-Cross/archive/$VERS.tar.gz
tar xvf $VERS.tar.gz
pushd SPIRV-Cross-$VERS
mkdir build
pushd build
cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=$PREFIX \
    -DCMAKE_POSITION_INDEPENDENT_CODE=on \
    -DSPIRV_CROSS_ENABLE_TESTS=off \
    ..
make -j $(nproc)
make install
popd # build
popd # SPIRV-Cross-$VERS
popd # spirv-cross

# Vulkan
install_vulkan

# GLM (GL Mathematics)
install_glm

# OpenSAML
install_xerces_c
download_make_install ${HTTP_DEPS}/xml-security-c-2.0.4.tar.gz "" "--without-xalan --enable-static --disable-shared"
download_make_install ${HTTP_DEPS}/xmltooling-3.0.4-nolog4shib.tar.gz "" "--enable-static --disable-shared"
CXXFLAGS="-std=c++14" download_make_install ${HTTP_DEPS}/opensaml-3.0.1-nolog4shib.tar.gz "" "--enable-static --disable-shared"

# H3
install_h3

sed -e "s|%MAPD_DEPS_ROOT%|$PREFIX|g" ${SCRIPTS_DIR}/mapd-deps.modulefile.in > mapd-deps-$SUFFIX.modulefile
sed -e "s|%MAPD_DEPS_ROOT%|$PREFIX|g" ${SCRIPTS_DIR}/mapd-deps.sh.in > mapd-deps-$SUFFIX.sh


cp mapd-deps-$SUFFIX.sh mapd-deps-$SUFFIX.modulefile $PREFIX

if [ "$COMPRESS" = "true" ] ; then
    OS=rockylinux8
      TARBALL_TSAN=""
    if [ "$TSAN" = "true" ]; then
      TARBALL_TSAN="-tsan"
    fi
    FILENAME=mapd-deps-${OS}${TARBALL_TSAN}-static-${ARCH}-${SUFFIX}.tar
    tar -cvf ${FILENAME} -C $(dirname $PREFIX) $SUFFIX
    pxz ${FILENAME}
fi

echo "Finished!!!"
