#!/data/data/com.termux/files/usr/bin/bash

# Set the countdown timer (e.g., 60 seconds)
countdown=60

# Function to display countdown in hh:mm:ss format
function display_countdown() {
    local hours=$((countdown / 3600))
    local minutes=$(( (countdown % 3600) / 60 ))
    local seconds=$((countdown % 60))
    echo -ne "Time remaining: $hours:$minutes:$seconds\r"
}

# Start time
start_time=$(date +%s)

# Display a waiting message
echo "Executing... Please wait."

# Execute the commands
pkg update -y
clear
# Display a waiting message
echo "Executing... Please wait."
pkg install x11-repo -y > /dev/null 2>&1
pkg install proot-distro proot termux-x11-nightly wget git pulseaudio -y > /dev/null 2>&1
termux-setup-storage > /dev/null 2>&1
proot-distro install debian > /dev/null 2>&1
wget https://raw.githubusercontent.com/mebabo1/menano/File/x11 > /dev/null 2>&1
wget https://raw.githubusercontent.com/mebabo1/menano/File/start-droidbox > /dev/null 2>&1
chmod +x start-droidbox && chmod +x x11 > /dev/null 2>&1
cp -r start-droidbox x11 /data/data/com.termux/files/usr/bin > /dev/null 2>&1
rm -rf x11 > /dev/null 2>&1

# Countdown timer synchronized with execution
while [ $countdown -gt 0 ]; do
    display_countdown
    sleep 1
    ((countdown--))
done

# Clear the countdown message
echo -ne "\r\033[K"

# Completion message
echo "Execution completed successfully!"
