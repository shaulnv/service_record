#
# Script to install & activate python virtual env with all the deps needed
# run at each bash-login: source ./activate-local-env.sh
# after script runs to complete, see usage()
#

activate_usage() {
    echo "Usage:"
    echo "  all your dev env tools are in the path (cmake, conan, etc.)."
    echo "  if you need to call 'sudo ...' use 'venv-sudo ...' instead."
}

# function to check if the script is sourced
_ensure_sourced() {
    if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
        echo "Error: This script must be sourced, not executed."
        echo "Please run: source ${BASH_SOURCE[0]}"
        exit 1
    fi
}

_get_latest_python_version() {
    # Find all Python executables
    python_versions=$(ls /usr/bin/python3* 2>/dev/null | grep -Eo 'python3\.[0-9]+' | sort -V)

    # Extract the latest version
    latest_version=$(echo "$python_versions" | tail -n 1 | grep -Eo '3\.[0-9]+')

    # Print the result
    if [ -n "$latest_version" ]; then
        echo "python$latest_version"
    else
        # No Python 3.x versions found, so let it failed normaly
        echo python3
    fi
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root_dir=$script_dir
red="\033[0;31m"
green="\033[0;32m"
nc='\033[0m' # No Color
python_exe=$(_get_latest_python_version)

# function to run any program as sudo, inside the virtual env
#   e.g. if you have a test which need sudo, use venv-sudo ./regression.py ...
alias venv-sudo='sudo -E env "PATH=$PATH" "VIRTUAL_ENV=$VIRTUAL_ENV"'

# we need this script sourced, as it activate python venv, which itself need to be sourced
_ensure_sourced

if [[ "$1" == "--quiet" || "$1" == "-q" ]]; then
    quiet=true
fi

# install virtual env
venv_dir=$root_dir/.venv
(
    # exit on command failure
    set -eE
    set -o pipefail
    trap 'deactivate &> /dev/null; rm -rf $venv_dir' ERR

    # install the venv only once
    if [[ ! -d $venv_dir ]]; then
        echo -e "${green}Creating a ${python_exe}'s Virtual Env...${nc}"
        $python_exe -m venv $venv_dir
        source $venv_dir/bin/activate
        pip install --upgrade pip
        pip install -r $root_dir/env/requirements.txt
        # conan setup
        # this gets conan the build defs: gcc version, os, etc.
        conan profile detect -e
    fi
) || true

if [[ ! -d $venv_dir ]]; then
    echo -e "${red}ERROR: Failed creating a Python's Virtual Env.${nc}"
    return
fi

# activate vitural env (if not activated alreay)
if [[ ! -n "$VIRTUAL_ENV" ]]; then
    source $venv_dir/bin/activate
fi

# cache 3rd parties CMake projects, imported by CPM
# you can set this env var in your .bashrc to a more persistant place like $HOME/.cache/CPM
if [[ ! -n "$CPM_SOURCE_CACHE" ]]; then
    export CPM_SOURCE_CACHE=$root_dir/.cache/CPM
fi

[ ! -n "$quiet" ] && echo "Virtual Env was loaded"
[ ! -n "$quiet" ] && activate_usage

