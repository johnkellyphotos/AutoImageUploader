# Install project

cd ~/Desktop/
# wget ...... 
# unzip ....
cd AutoImageUploader/
make

# set service to run



#!/bin/bash

# Update and install necessary libraries
sudo apt update
sudo apt upgrade -y
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libusb-1.0-0-dev libjson-c-dev libgphoto2-dev gphoto2

# Build project
cd ~/Desktop/
# wget project ...... 
# unzip ....
cd AutoImageUploader/
make

# Set up systemd service
sudo tee /etc/systemd/system/autoimageuploader.service > /dev/null <<EOF
[Unit]
Description=Auto Image Uploader
After=network.target

[Service]
Type=simple
ExecStart=%h/Desktop/AutoImageUploader/gui_uploader
Restart=on-failure
User=$USER
WorkingDirectory=%h/Desktop/AutoImageUploader

[Install]
WantedBy=default.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable autoimageuploader.service
sudo systemctl start autoimageuploader.service
