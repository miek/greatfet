#!/usr/bin/env python3
#
# This file is part of GreatFET

from __future__ import print_function

import argparse
import cv2
import errno
import numpy as np
import sys
import time

import greatfet
from greatfet import GreatFET
from greatfet.protocol import vendor_requests
from greatfet.utils import log_silent, log_verbose

def find_image_start(buf):
    found = False
    try:
        for i in range(len(buf)-7):
            if buf[i:i+8] == b'\xff\x7f\xff\x7f\xff\x7f\xff\x7f':
                found = True

            if found and buf[i] != 0xff and buf[i] != 0x7f:
                return i-2

    except Exception:
       pass
    return -1


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
                buf = b''
                level = 20000
                gain = 10.0
                sync = False
                while True:
                    rd = device.taxi.read()
                    buf += rd
                    height = 245
                    width = 327

                    if len(buf) >= height*width*4:
                        idx = find_image_start(buf)
                        if idx >= 0:
                            sync = True
                            buf = buf[idx:]
                        else:
                            buf = b''

                    if len(buf) >= height*width*2 and sync:
                        sync = False
                        d = np.frombuffer(buf[0:height*width*2], dtype=np.uint16).reshape(height, width)
                        buf = buf[height*width*2:]
                        if d[240][326] != 0x7fff:
                            continue

                        im = (((d-level)*gain)/256).astype(np.uint8)
                        im = cv2.applyColorMap(im, cv2.COLORMAP_RAINBOW)

                        # Display the resulting frame
                        cv2.imshow('frame', im)
                        key = cv2.waitKey(1) & 0xFF
                        if key == ord('q'):
                            break
                        elif key == ord('a'):
                            gain -= 0.1
                        elif key == ord('d'):
                            gain += 0.1
                        elif key == ord('w'):
                            level -= 100
                        elif key == ord('s'):
                            level += 100
            except KeyboardInterrupt:
                pass
        device.taxi.stop()
        cv2.destroyAllWindows()


if __name__ == '__main__':
    main()
