#!/bin/sh


#
# Defining some variables
#

PROJECT_NAME=hclib

INSTALL_PREFIX="${PWD}/${PROJECT_NAME}-install"

check_error()
{
    if [ $# -gt 2 ]; then
	echo "Error in check_error call";
	exit 1;
    fi;
    ERRCODE="$1";
    if [ "$ERRCODE" = "0" ]; then
	return 0;
    fi;
    if [ $# -eq 2 ]; then
	ERRMESSAGE="$2";
    else
	ERRMESSAGE="Error";
    fi;
    echo "[${PROJECT_NAME}] $ERRMESSAGE";
    exit $ERRCODE;
}


#
# Bootstrap, Configure, Make and Install
#

if [ -z "$NPROC" ]; then 
    NPROC=1
fi

#
# Bootstrap
#
# if install root has been specified, add --prefix option to configure
if [ -n "${INSTALL_ROOT}" ]; then
    INSTALL_ROOT="--prefix=${INSTALL_ROOT}"
else
    INSTALL_ROOT="--prefix=${PWD}/${PROJECT_NAME}-install"
fi

echo "[${PROJECT_NAME}] Bootstrap..."

./bootstrap.sh
check_error "$?" "Bootstrap failed";


#
# Configure
#
echo "[${PROJECT_NAME}]] Configure..."

COMPTREE=$PWD/compileTree
if [ ! -d "${COMPTREE}" ]; then
    mkdir ${COMPTREE}
fi

cd ${COMPTREE}

../configure ${INSTALL_ROOT} ${HCUPC_FLAGS} ${HCLIB_FLAGS}
check_error "$?" "Configure failed";


#
# Make
#
echo "[${PROJECT_NAME}]] Make..."
make -j${NPROC}
check_error "$?" "Build failed";

#
# Make install
#
# if install root has been specified, perform make install
echo "[${PROJECT_NAME}]] Make install... to ${INSTALL_ROOT}"
make -j${NPROC} install
check_error "$?" "Installation failed";


echo "[${PROJECT_NAME}]] Installation complete."

HCLIB_ENV_SETUP_SCRIPT=${INSTALL_PREFIX}/bin/hclib_setup_env.sh

mkdir -p `dirname ${HCLIB_ENV_SETUP_SCRIPT}`
cat > "${HCLIB_ENV_SETUP_SCRIPT}" <<EOI
# HClib environment setup
export HCLIB_ROOT='${INSTALL_PREFIX}'
EOI

cat <<EOI
[${PROJECT_NAME}] Installation complete.

${PROJECT_NAME} installed to: ${INSTALL_PREFIX}

Add the following to your .bashrc (or equivalent):
source ${HCLIB_ENV_SETUP_SCRIPT}
EOI

