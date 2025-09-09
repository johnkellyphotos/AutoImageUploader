#!/bin/bash

# Update and install necessary libraries
sudo apt update
sudo apt upgrade -y
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libusb-1.0-0-dev libjson-c-dev libgphoto2-dev gphoto2

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

# hide cursor
sudo apt install unclutter
unclutter -display :0 -idle 0

# autostart hide cursor
mkdir -p ~/.config/lxsession/LXDE-pi/
echo "@unclutter -display :0 -idle 0" >> ~/.config/lxsession/LXDE-pi/autostart

sudo apt update && sudo apt upgrade -y
