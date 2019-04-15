import sys
import time
import matplotlib
matplotlib.use ('Agg')
import numpy
import matplotlib.pyplot as plt

from fps_inspector_sdk import fps_inspector

def main ():
    float_formatter = lambda x: "%.5f" % x
    numpy.set_printoptions (formatter={'float_kind': float_formatter}, threshold=numpy.inf)

    pid = int (sys.argv[1])
    fps_inspector.start_fliprate_recording (pid)
    time.sleep (10)
    fps_inspector.stop_fliprate_recording ()
    data = fps_inspector.get_all_fliprates ()
    print (data)

    plt.figure ()
    data[data.ScreenTime != 0][['FPS', 'FlipRate', 'ScreenTime']].plot (x='ScreenTime', subplots=True)
    plt.savefig ('plot.png')
    data.to_csv('scores.csv')
    plt.close ()


if __name__ == "__main__":
    main ()
