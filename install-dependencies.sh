#!/usr/bin/env bash
#
# Robust, portable dependency installer for Linux distributions.
# Installs: build-essential (gcc, make, etc.), zlib, openssl, libffi, ncurses.
#

# --- Script Configuration ---
# set -e: Exit immediately if a command exits with a non-zero status.
# set -u: Treat unset variables as an error.
# set -o pipefail: The return value of a pipeline is the status of
#                  the last command to exit with a non-zero status.
set -euo pipefail

# --- Logging Functions ---
# (Used to add color and structure)
log_error() {
    echo >&2 -e "\e[31m[ERROR]\e[0m $1"
}

log_info() {
    echo -e "\e[34m[INFO]\e[0m $1"
}

log_success() {
    echo -e "\e[32m[SUCCESS]\e[0m $1"
}

# --- Package Manager Install Functions ---

install_apt() {
    log_info "Updating package lists..."
    apt-get update
    log_info "Installing dependencies..."
    apt-get install -y \
        build-essential \
        gcc \
        zlib1g-dev \
        libssl-dev \
        libffi-dev \
        libncurses5-dev \
        libncursesw5-dev
}

install_dnf() {
    log_info "Installing dependencies..."
    dnf install -y \
        '@Development Tools' \
        gcc \
        zlib-devel \
        openssl-devel \
        libffi-devel \
        ncurses-devel
}

install_yum() {
    log_info "Installing dependencies..."
    yum install -y \
        '@Development Tools' \
        gcc \
        zlib-devel \
        openssl-devel \
        libffi-devel \
        ncurses-devel
}

install_pacman() {
    log_info "Updating package database..."
    pacman -Syu --noconfirm
    log_info "Installing dependencies..."
    pacman -S --noconfirm --needed \
        base-devel \
        gcc \
        zlib \
        openssl \
        libffi \
        ncurses
}

install_zypper() {
    log_info "Refreshing repositories..."
    zypper refresh
    log_info "Installing dependencies..."
    zypper install -y --type pattern \
        devel_basis
    zypper install -y \
        gcc \
        zlib-devel \
        libopenssl-devel \
        libffi-devel \
        ncurses-devel
}

install_apk() {
    log_info "Updating package lists..."
    apk update
    log_info "Installing dependencies..."
    apk add \
        build-base \
        gcc \
        zlib-dev \
        openssl-dev \
        libffi-dev \
        ncurses-dev
}

# --- Main Execution ---

main() {
    if [ "$EUID" -ne 0 ]; then
        log_error "This script must be run as root or with sudo."
        exit 1
    fi


    if command -v apt-get &> /dev/null; then
        log_info "Detected Debian-based system (apt-get)."
        install_apt
    elif command -v dnf &> /dev/null; then
        log_info "Detected RHEL-based system (dnf)."
        install_dnf
    elif command -v yum &> /dev/null; then
        log_info "Detected RHEL-based system (yum)."
        install_yum
    elif command -v pacman &> /dev/null; then
        log_info "Detected Arch-based system (pacman)."
        install_pacman
    elif command -v zypper &> /dev/null; then
        log_info "Detected openSUSE-based system (zypper)."
        install_zypper
    elif command -v apk &> /dev/null; then
        log_info "Detected Alpine-based system (apk)."
        install_apk
    else
        log_error "Could not detect a supported package manager."
        log_error "Please manually install the following dependencies for your system:"
        echo "  - C/C++ Build Toolchain (gcc, make, etc.)"
        echo "  - zlib (development headers)"
        echo "  - openssl (development headers)"
        echo "  - libffi (development headers)"
        echo "  - ncurses (development headers)"
        exit 1
    fi

    log_success "All dependencies installed successfully."
}

main