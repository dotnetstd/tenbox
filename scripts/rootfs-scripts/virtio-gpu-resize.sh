#!/bin/bash
# Auto-resize display when virtio-gpu hotplug event occurs

sleep 0.1
export DISPLAY=:0

# Try to find valid XAUTHORITY
TENBOX_UID=$(id -u tenbox 2>/dev/null || echo 1000)
for auth in /home/tenbox/.Xauthority /var/run/lightdm/tenbox/:0 /run/user/${TENBOX_UID}/gdm/Xauthority; do
    if [ -f "$auth" ]; then
        export XAUTHORITY="$auth"
        break
    fi
done

for output in Virtual-1 Virtual-0; do
    xrandr --output "$output" --auto 2>/dev/null && break
done
