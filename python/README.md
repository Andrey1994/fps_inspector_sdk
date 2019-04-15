## FPS Inspector SDK

It's a library which allows you to measure FPS FlipRate and other metrics using python code.

It's based on Event Tracing and doesn't hook inside game process unlike Fraps

Simple Sample:
```
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
    data = fps_inspector.get_all_fliprates () # get all data from begining, also you can check the latest data using get_last_fliprates(num_samples) method
    # data is a pandas dataframe, it simplify data analysis!
    print (data)

    plt.figure ()
    data[data.ScreenTime != 0][['FPS', 'FlipRate', 'ScreenTime']].plot (x='ScreenTime', subplots=True)
    plt.savefig ('plot.png')
    data.to_csv('scores.csv')
    plt.close ()


if __name__ == "__main__":
    main ()
```