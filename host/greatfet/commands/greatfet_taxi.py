#!/usr/bin/env python3
#
# This file is part of GreatFET

from __future__ import print_function

import argparse
import errno
import sys
import time

import greatfet
from greatfet import GreatFET
from greatfet.protocol import vendor_requests
from greatfet.utils import log_silent, log_verbose


def main():
    logfile = 'log.bin'
#    logfile = '/tmp/fifo'
    # Set up a simple argument parser.
    parser = argparse.ArgumentParser(description="Utility for TAXI")
    parser.add_argument('-s', dest='serial', metavar='<serialnumber>', type=str,
                        help="Serial number of device, if multiple devices", default=None)
    parser.add_argument('-S', dest='samplerate', metavar='<samplerate>', type=int,
                        help="Sample rate for IR Tx", default=1000000)
    parser.add_argument('-f', dest='filename', metavar='<filename>', type=str, help="File to read or write", default=logfile)
    parser.add_argument('-r', dest='receive', action='store_true', help="Write data to file")
    parser.add_argument('-R', dest='repeat', action='store_true', help="Repeat file data (tx only)")
    parser.add_argument('-v', dest='verbose', action='store_true', help="Increase verbosity of logging")
    args = parser.parse_args()

    log_function = log_verbose if args.verbose else log_silent

    try:
        log_function("Trying to find a GreatFET device...")
        device = GreatFET(serial_number=args.serial)
        log_function("{} found. (Serial number: {})".format(device.board_name(), device.serial_number()))
    except greatfet.errors.DeviceNotFoundError:
        if args.serial:
            print("No GreatFET board found matching serial '{}'.".format(args.serial), file=sys.stderr)
        else:
            print("No GreatFET board found!", file=sys.stderr)
        sys.exit(errno.ENODEV)

    if args.receive:
        device.taxi.start_receive()
        time.sleep(1)
        with open(args.filename, 'wb') as f:
            try:
                while True:
                    d = device.taxi.read()
                    # print(d)
                    f.write(d)
            except KeyboardInterrupt:
                pass
        device.taxi.stop()


if __name__ == '__main__':
    main()
