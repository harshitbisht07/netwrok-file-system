#!/bin/bash
# launch_all.sh - Launch all NFS components in separate terminals
#
# This script launches:
#   1. Naming Server (port 5566 for SS, 5568 for clients)
#   2. Storage Server (port 5567)
#   3. Client (interactive)
#
# Usage: ./launch_all.sh

set -e

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}╔════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   Network File System Launcher             ║${NC}"
echo -e "${CYAN}╚════════════════════════════════════════════╝${NC}"
echo ""

# Shared secret: every component must use the same one.
# Use your own with:  NFS_SECRET='...' ./launch_all.sh   (otherwise a random one is made)
if [ -z "${NFS_SECRET:-}" ]; then
    NFS_SECRET="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
fi
export NFS_SECRET
echo -e "${YELLOW}Shared secret for this session: ${NFS_SECRET}${NC}"
echo -e "${YELLOW}(use it for clients on other PCs: NFS_SECRET=... ./client <NM_IP> 5568)${NC}"
echo ""

# Check if executables exist
if [ ! -f "./naming-server" ] || [ ! -f "./server" ] || [ ! -f "./client" ]; then
    echo -e "${RED}Error: Executables not found!${NC}"
    echo -e "${YELLOW}Building components...${NC}"
    bash bash.sh
    echo ""
fi

# Detect terminal emulator
detect_terminal() {
    if command -v gnome-terminal &> /dev/null; then
        echo "gnome-terminal"
    elif command -v xterm &> /dev/null; then
        echo "xterm"
    elif command -v konsole &> /dev/null; then
        echo "konsole"
    elif command -v xfce4-terminal &> /dev/null; then
        echo "xfce4-terminal"
    elif command -v powershell.exe &> /dev/null; then
        echo "powershell"
    else
        echo "none"
    fi
}

TERMINAL=$(detect_terminal)

# Launch function based on terminal type
launch_in_terminal() {
    local title="$1"
    local command="$2"
    
    case "$TERMINAL" in
        gnome-terminal)
            gnome-terminal --title="$title" -- bash -c "$command; echo ''; echo 'Press Enter to close...'; read" &
            ;;
        xterm)
            xterm -T "$title" -e bash -c "$command; echo ''; echo 'Press Enter to close...'; read" &
            ;;
        konsole)
            konsole --title "$title" -e bash -c "$command; echo ''; echo 'Press Enter to close...'; read" &
            ;;
        xfce4-terminal)
            xfce4-terminal --title="$title" -e "bash -c \"$command; echo ''; echo 'Press Enter to close...'; read\"" &
            ;;
        powershell)
            # For WSL/Windows - launch in PowerShell windows
            powershell.exe -Command "Start-Process powershell -ArgumentList '-NoExit', '-Command', 'cd \"$SCRIPT_DIR\"; bash -c \"$command\"'" &
            ;;
        *)
            echo -e "${RED}Error: No supported terminal emulator found!${NC}"
            echo -e "${YELLOW}Please install one of: gnome-terminal, xterm, konsole, or xfce4-terminal${NC}"
            echo ""
            echo -e "${YELLOW}Alternatively, run these commands manually in separate terminals:${NC}"
            echo -e "  (first: export NFS_SECRET='<same secret everywhere>')"
            echo -e "  Terminal 1: ${GREEN}./naming-server${NC}"
            echo -e "  Terminal 2: ${GREEN}./server 127.0.0.1 5566 5567 ./storage${NC}"
            echo -e "  Terminal 3: ${GREEN}./client 127.0.0.1 5568${NC}"
            exit 1
            ;;
    esac
}

echo -e "${GREEN}[1/3] Launching Naming Server...${NC}"
launch_in_terminal "NFS - Naming Server" "NFS_SECRET='$NFS_SECRET' ./naming-server 0.0.0.0 5566 5568 --remote-rw"
sleep 2

echo -e "${GREEN}[2/3] Launching Storage Server...${NC}"
launch_in_terminal "NFS - Storage Server" "NFS_SECRET='$NFS_SECRET' ./server 127.0.0.1 5566 5567 ./storage"
sleep 2

echo -e "${GREEN}[3/3] Launching Client...${NC}"
launch_in_terminal "NFS - Client" "NFS_SECRET='$NFS_SECRET' ./client 127.0.0.1 5568"
sleep 1

echo ""
echo -e "${GREEN}✓ All components launched in separate terminals!${NC}"
echo ""
echo -e "${YELLOW}Launch order:${NC}"
echo -e "  ${CYAN}1.${NC} Naming Server (Port 5566 & 5568)"
echo -e "  ${CYAN}2.${NC} Storage Server (Port 5567)"
echo -e "  ${CYAN}3.${NC} Client (connects to 5568)"
echo ""
echo -e "${CYAN}Look for 3 new terminal windows on your screen!${NC}"
echo ""
echo -e "${YELLOW}To stop all servers, close the terminal windows or press Ctrl+C in each.${NC}"
