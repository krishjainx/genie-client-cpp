#!/bin/bash
cd /opt/genie
export TZ=UTC
if [ -z "$(pidof ntpd)" ]; then
	/usr/bin/start_ntp.sh &
fi
amixer -D hw:audiocodec cset name='LINEOUT volume' 8
amixer -D hw:audiocodec cset name='MIC1 gain volume' 6
amixer -D hw:audiocodec cset name='MIC2 gain volume' 6

export GIO_EXTRA_MODULES=lib/gio/modules
export LD_LIBRARY_PATH=lib/:/lib:/usr/lib
export GST_REGISTRY_UPDATE=no
export GST_PLUGIN_PATH=/opt/genie/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/opt/genie/lib/gstreamer-1.0/gst-plugin-scanner
sleep 5
./genie