import curses, sys

try:
    try:
        curses.initscr()

        print 1
        print curses.version
        print curses.longname()
        print curses.baudrate()
        print curses.can_change_color()
        curses.start_color()
        print curses.color_pair(curses.A_BLINK)
    finally:
        curses.endwin()
except:
    print sys.exc_info()[1]
