#!/bin/bash

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root_dir=$(realpath "$script_dir/..")

green="\033[32m"
no_color="\033[0m"

# Function to check if the script is sourced
_ensure_sourced() {
  if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "Error: This script must be sourced, not executed."
    echo "Please run: source ${BASH_SOURCE[0]}"
    exit 1
  fi
}

# Function to add or replace a key=value pair in an .env file
_update_env_file() {
  local file=$1
  local key=$2
  local value=$3

  # Check if key already exists in the file
  if grep -q "^$key=" "$file" >/dev/null 2>&1; then
    # If key exists, replace the value using | as delimiter to avoid issues with paths
    sed -i "s|^$key=.*|$key=$value|" "$file"
  else
    # If key does not exist, add it to the end of the file
    echo "$key=$value" >>"$file"
  fi
}

# Function to display usage information
build_usage() {
  echo "Usage: $0 [OPTIONS]"
  echo "Options:"
  echo "  --release               Build Release"
  echo "  --debug                 Build Debug"
  echo "  --profile <native|wasm> Build to a spesific architecture"
  echo "  --clean                 Clean the build folder"
  exit 0
}

# Function to parse command-line arguments and export environment variables
_parse_command_line() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
    -h | --help)
      build_usage
      ;;
    --release)
      build_type=Release
      ;;
    --debug)
      build_type=Debug
      ;;
    --profile)
      shift
      if [[ -z "$1" || "$1" == --* ]]; then
        echo "Error: Missing value for --profile"
        exit 1
      fi
      profile="$1"
      ;;
    --clean)
      clean=true
      ;;
    --*=*)
      # Handle arbitrary --option=value
      option="${1%%=*}"     # Extract option name
      value="${1#*=}"       # Extract value
      varname="${option:2}" # Strip leading --
      export "$varname=$value"
      ;;
    --*)
      # Handle arbitrary --option value
      option="$1"
      shift
      if [[ -z "$1" || "$1" == --* ]]; then
        echo "Error: Missing value for $option"
        exit 1
      fi
      varname="${option:2}" # Strip leading --
      export "$varname=$1"
      ;;
    *)
      echo "Error: Invalid argument $1"
      exit 1
      ;;
    esac
    shift
  done
}

function _create_env_vars_file() {
  # env vars
  # default values --> read from .env file --> take from command line
  local build_type=Debug
  local profile=native
  source $root_dir/.env >/dev/null 2>&1 || true
  local stored_profile=$profile
  _parse_command_line "$@"

  # if the profile has changed, we need a clean build
  if [[ -n "$stored_profile" && "$stored_profile" != "$profile" ]]; then
    echo -e "${green}Profile changed, will do a clean build${no_color}"
    clean=true
  fi

  # set the cli name and driver based on the profile
  if [[ "$profile" == "wasm" ]]; then
    local cli_name="service_record.js"
    local driver=node
  else
    local cli_name="service_record"
    local driver=
  fi

  local build_folder=$root_dir/build/$build_type
  local cli_path="$build_folder/cli/$cli_name"

  # make the env vars persistent, so next time, we won't need to pass the command line flags
  _update_env_file $root_dir/.env build_type $build_type
  _update_env_file $root_dir/.env profile $profile
  _update_env_file $root_dir/.env build_folder $build_folder
  _update_env_file $root_dir/.env cli_name $cli_name
  _update_env_file $root_dir/.env cli_path $cli_path
  _update_env_file $root_dir/.env driver $driver
}

# Function to load environment variables from .env file
_load_env_vars() {
  source $root_dir/.env >/dev/null 2>&1 || true
}

# Function to activate python virtual environment
_activate_venv() {
  source $root_dir/activate.sh --quiet
}

# start
_ensure_sourced

_activate_venv
_create_env_vars_file "$@"
_load_env_vars
