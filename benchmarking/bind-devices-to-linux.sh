#!/bin/sh
# Ensures that the devices given as $@, but not necessarily only those, are bound to a Linux driver and 'up'.
# (Unlike the UIO script, we don't care if too many devices are bound because the NF is expected to know)
# Devices given as $@ may but do not need to have the domain (e.g. '83:00.0' and '0000:83:00.0' are equivalent assuming the domain is 0)

# Note that binding/unbinding devices occasionally has weird effects like killing SSH sessions,
# so we should only do it if absolutely necessary

# List all PCI devices
# notes about lspci:
# -D to force the domain even if there is only one on the machine (sysfs needs it)
# -mm to be machine-readable
# -vk to show all info including drivers
# 10 lines is enough to cover all info of a single device
for pci in $(lspci -mm | cut -d ' ' -f 1 | tr '\n' ' '); do
  # Get PCI addr with domain
  full_pci="$(lspci -D | grep -F "$pci" | cut -d ' ' -f 1)"
  # Get driver
  driver="$(lspci -vmmk | grep -F "$pci" -A 10 | grep Driver | cut -f 2)"
  # Bind or unbind as needed
  case "$driver" in
    ixgbe)
      # OK, already bound, find
      ;;
    *)
      if echo "$@" | grep -Fq "$pci" ; then
        # Not bound to Linux but should be, unbind from current driver if it has one then bind to Linux
        if [ -e "/sys/bus/pci/devices/$full_pci/driver" ]; then
          echo -n "$full_pci" | sudo tee "/sys/bus/pci/devices/$full_pci/driver/unbind" >/dev/null 2>&1
        fi
        echo 'ixgbe' | sudo tee "/sys/bus/pci/devices/$full_pci/driver_override" >/dev/null 2>&1
        echo -n "$full_pci" | sudo tee "/sys/bus/pci/drivers/ixgbe/bind" >/dev/null 2>&1
        name=$(ls -1 "/sys/bus/pci/devices/$full_pci/net/")
        sudo ip link set dev "$name" up
      fi
      ;;
  esac
done
