#!/bin/bash


# Update and install necessary libraries
sudo apt update
sudo apt upgrade -y
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libusb-1.0-0-dev libjson-c-dev libgphoto2-dev gphoto2 onboard unclutter


# Set Raspberry Pi to autologin to desktop
sudo sed -i '/^\[Seat:\*\]/a autologin-user=pi\nautologin-session=lightdm-autologin' /etc/lightdm/lightdm.conf 2>/dev/null


# Ensure desktop autologin via raspi-config
sudo raspi-config nonint do_boot_behaviour B4

# Build project
cd ~/Desktop/
# wget project ...... 
# unzip ....
cd AutoImageUploader/
make


# Copy the existing service file to systemd
sudo cp autoimageuploader.service /etc/systemd/system/


# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable autoimageuploader.service
sudo systemctl start autoimageuploader.service


# Hide cursor
unclutter -display :0 -idle 0 &


# Autostart hide cursor
mkdir -p /home/pi/.config/lxsession/LXDE-pi/
echo "@unclutter -display :0 -idle 0" >> /home/pi/.config/lxsession/LXDE-pi/autostart


# Disable network notifications that popup over UI
sudo -u pi gsettings set org.gnome.nm-applet show-notifications false


# Init onboard to make onscreen keyboard possible
sudo -u pi dconf write /org/onboard/preferences/general/auto-show true
# to launch: `onboard &`


# Prevent RaspberryPi from auto mounting camera (interfers with gphoto2)
# Write udev rule to ignore cameras only
cat <<EOF | sudo tee /etc/udev/rules.d/99-gphoto2.rules
ATTR{idVendor}=="04a9", ATTR{idProduct}=="3110", ENV{ID_GPHOTO2}="1", ENV{UDISKS_IGNORE}="1"
EOF


# Reload udev
sudo udevadm control --reload
sudo udevadm trigger