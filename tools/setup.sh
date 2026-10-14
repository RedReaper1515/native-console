#!/bin/bash

# Native Console Setup Script for macOS/Linux

set -e

echo "=========================================="
echo "Native Console - Setup Script"
echo "=========================================="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check OS
OS=$(uname)

echo -e "Detected OS: ${YELLOW}$OS${NC}"

# Function to install dependencies based on OS
install_dependencies() {
    if [[ "$OS" == "Darwin" ]]; then
        # macOS
        echo -e "\n${GREEN}Installing macOS dependencies...${NC}"
        if ! command -v brew &> /dev/null; then
            echo -e "${RED}Homebrew not found! Please install Homebrew first:${NC}"
            echo "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
            exit 1
        fi
        
        brew update
        brew install cmake ninja sdl2 python3 pipx
        
        echo -e "\n${GREEN}Installing Python dependencies...${NC}"
        pip3 install pyboy
    elif [[ "$OS" == "Linux" ]]; then
        # Linux
        echo -e "\n${GREEN}Installing Linux dependencies...${NC}"
        if command -v apt-get &> /dev/null; then
            # Debian/Ubuntu
            sudo apt-get update
            sudo apt-get install -y cmake ninja-build libsdl2-dev python3-pip
        elif command -v yum &> /dev/null; then
            # CentOS/RHEL
            sudo yum install -y cmake ninja-build SDL2-devel python3-pip
        elif command -v pacman &> /dev/null; then
            # Arch Linux
            sudo pacman -Syu --noconfirm cmake ninja sdl2 python-pip
        else
            echo -e "${RED}Unsupported package manager! Please install dependencies manually.${NC}"
            echo "Required: cmake, ninja, libsdl2-dev, python3-pip"
            exit 1
        fi
        
        echo -e "\n${GREEN}Installing Python dependencies...${NC}"
        pip3 install --upgrade pip
        pip3 install pyboy
    else
        echo -e "${RED}Unsupported OS! This script supports macOS and Linux only.${NC}"
        exit 1
    fi
}

# Function to build the recompiler
build_recompiler() {
    echo -e "\n${GREEN}Building the recompiler...${NC}"
    
    if [ ! -d "build" ]; then
        mkdir build
    fi
    
    cd build
    cmake -G Ninja ..
    
    echo -e "\n${GREEN}Compiling...${NC}"
    ninja
    
    cd ..
    
    echo -e "\n${GREEN}Build completed successfully!${NC}"
    echo "Recompiler executable: ${YELLOW}./build/bin/gbrecomp${NC}"
}

# Main script
main() {
    echo -e "\n${GREEN}Step 1/3: Checking and installing dependencies...${NC}"
    install_dependencies
    
    echo -e "\n${GREEN}Step 2/3: Building the recompiler...${NC}"
    build_recompiler
    
    echo -e "\n${GREEN}Step 3/3: Verifying installation...${NC}"
    if [ -f "build/bin/gbrecomp" ]; then
        echo -e "${GREEN}✓ Recompiler executable found${NC}"
    else
        echo -e "${RED}✗ Recompiler executable not found!${NC}"
        exit 1
    fi
    
    echo -e "\n${GREEN}Setup complete!${NC}"
    echo -e "\nTo recompile a ROM:"
    echo -e "  ./build/bin/gbrecomp <rom.gb> -o <output_dir>"
    echo -e "\nFor the full ground truth workflow, see ${YELLOW}docs/GROUND_TRUTH_WORKFLOW.md${NC}"
    echo -e "\nOr use the automated script:"
    echo -e "  python3 tools/run_ground_truth.py <rom.gb>"
}

# Check if script is being run from the correct directory
if [ ! -f "CMakeLists.txt" ]; then
    echo -e "${RED}Error: This script must be run from the root of the native-console repository!${NC}"
    exit 1
fi

# Parse command line options
while getopts ":h" opt; do
    case $opt in
        h)
            echo "Native Console Setup Script"
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -h  Show this help message"
            exit 0
            ;;
        \?)
            echo "Invalid option: -$OPTARG" >&2
            exit 1
            ;;
    esac
done

# Run the main function
main
