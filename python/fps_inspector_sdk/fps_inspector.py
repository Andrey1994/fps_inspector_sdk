import os
import ctypes
import numpy
import pandas
from numpy.ctypeslib import ndpointer
import pkg_resources
import platform
import struct
from fps_inspector_sdk.exit_codes import PresentMonExitCodes


class FpsInspectorError (Exception):
    def __init__ (self, message, exit_code):
        detailed_message = '%s:%d %s' % (PresentMonExitCodes (exit_code).name, exit_code, message)
        super (FpsInspectorError, self).__init__ (detailed_message)
        self.exit_code = exit_code


class PresentMonDLL (object):

    __instance = None

    @classmethod
    def get_instance (cls):
        if cls.__instance is None:
            if platform.system () != 'Windows':
                raise Exception ("For now only Windows is supported, detected platform is %s" % platform.system ())
            if struct.calcsize ("P") * 8 != 64:
                raise Exception ("You need 64-bit python to use this library")
            cls.__instance = cls ()
        return cls.__instance

    def __init__ (self):

        self.lib = ctypes.cdll.LoadLibrary (pkg_resources.resource_filename (__name__, os.path.join ('lib', 'PresentMon.dll')))

        # start stream
        self.StartEventRecording = self.lib.StartEventRecording
        self.StartEventRecording.restype = ctypes.c_int64
        self.StartEventRecording.argtypes = [
            ctypes.c_int64,
            ctypes.c_int64
        ]

        # stop stream
        self.StopEventRecording = self.lib.StopEventRecording
        self.StopEventRecording.restype = ctypes.c_int64
        self.StopEventRecording.argtypes = []

        # set log level
        self.SetLogLevel = self.lib.SetLogLevel
        self.SetLogLevel.restype = ctypes.c_int
        self.SetLogLevel.argtypes = [
            ctypes.c_int64
        ]

        # get current data
        self.GetCurrentData = self.lib.GetCurrentData
        self.GetCurrentData.restype = ctypes.c_int64
        self.GetCurrentData.argtypes = [
            ctypes.c_int64,
            ndpointer (ctypes.c_double),
            ndpointer (ctypes.c_double),
            ndpointer (ctypes.c_int64)
        ]

        # get data count
        self.GetDataCount = self.lib.GetDataCount
        self.GetDataCount.restype = ctypes.c_int
        self.GetDataCount.argtypes = [
           ndpointer (ctypes.c_int64)
        ]

        # get data
        self.GetData = self.lib.GetData
        self.GetData.restype = ctypes.c_int
        self.GetData.argtypes = [
            ctypes.c_int64,
            ndpointer (ctypes.c_double),
            ndpointer (ctypes.c_double),
        ]


def start_fliprate_recording (pid = 0, max_samples = 86400*60):
    res = PresentMonDLL.get_instance ().StartEventRecording (pid, max_samples)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to start event tracing session', res)

def get_last_fliprates (num_samples):
    fliprate_arr = numpy.zeros (num_samples*6).astype (numpy.float64)
    time_arr = numpy.zeros (num_samples).astype (numpy.float64)
    current_size = numpy.zeros (1).astype (numpy.int64)

    res = PresentMonDLL.get_instance().GetCurrentData (num_samples, fliprate_arr, time_arr, current_size)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to get last fliprate data', res)
    sample_count = current_size[0]
    fliprate_arr = fliprate_arr[0:sample_count*6].reshape (sample_count, 6)
    return pandas.DataFrame (numpy.column_stack ((fliprate_arr, time_arr[0:current_size[0]])),
        columns=['FPS', 'FlipRate', 'DeltaReady', 'DeltaDisplayed', 'TimeTaken', 'ScreenTime', 'Timestamp'])

def get_immediate_fliprate ():
    fliprate = get_last_fliprates (1)
    if len (fliprate) > 0:
        return fliprate['FlipRate'][0]

def get_fliprate_count ():
    sample_count = numpy.zeros (1).astype (numpy.int64)

    res = PresentMonDLL.get_instance().GetDataCount (sample_count)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to get fliprate count', res)
    return sample_count[0]

def get_all_fliprates ():
    sample_count = get_fliprate_count ()
    current_size = numpy.zeros (1).astype (numpy.int64)
    current_size[0] = sample_count
    time_arr = numpy.zeros (sample_count).astype (numpy.float64)
    fliprate_arr = numpy.zeros (sample_count*6).astype (numpy.float64)

    res = PresentMonDLL.get_instance().GetData (current_size, time_arr, fliprate_arr)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to get fliprate data', res)

    fliprate_arr = fliprate_arr[0:sample_count*6].reshape (sample_count, 6)
    return pandas.DataFrame (numpy.column_stack ((fliprate_arr, time_arr[0:current_size[0]])),
        columns=['FPS', 'FlipRate', 'DeltaReady', 'DeltaDisplayed', 'TimeTaken', 'ScreenTime', 'Timestamp'])

def stop_fliprate_recording ():
    res = PresentMonDLL.get_instance ().StopEventRecording ()
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to stop fliprate capturing', res)

def enable_fliprate_log ():
    res = PresentMonDLL.get_instance ().SetLogLevel (0)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to enable fliprate log', res)

def disable_fliprate_log ():
    res = PresentMonDLL.get_instance ().SetLogLevel (6)
    if res != PresentMonExitCodes.STATUS_OK.value:
        raise FpsInspectorError ('unable to enable fliprate log', res)
