# Copyright 2020 Intel Corporation
# SPDX-License-Identifier: Apache 2.0
ifeq ($(HTTPPROXY), true)
DFLAGS += -DHTTPPROXY="\"10.223.4.20\"" -DHTTPPROXYPORT=911
endif

ifneq ($(WIFI_SSID), )
DFLAGS += -DWIFI_SSID=\"$(WIFI_SSID)\"
endif

ifneq ($(WIFI_PASS), )
DFLAGS += -DWIFI_PASS=\"$(WIFI_PASS)\"
endif

ifneq ($(MANUFACTURER_IP), )
DFLAGS += -DMANUFACTURER_IP=\"$(MANUFACTURER_IP)\"
endif

ifneq ($(MANUFACTURER_DN), )
DFLAGS += -DMANUFACTURER_DN=\"$(MANUFACTURER_DN)\"
endif
