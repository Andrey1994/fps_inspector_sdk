#pragma once
#include <windows.h>
#include <cstdlib>

template <class T>
class DataBuffer {

		CRITICAL_SECTION lock;

		double *timestamps;
		T *data;

		size_t bufferSize;
		size_t firstUsed, firstFree;
		size_t count;

		size_t next(size_t index) {
			return (index + 1) % bufferSize;
		}

		void getChunk(size_t start, size_t size, double *tsBuf, T *dataBuf);

	public:

		DataBuffer(size_t bufferSize);
		~DataBuffer();

		void addData(double timestamp, T data);
		size_t getData(size_t maxCount, double *tsBuf, T *dataBuf);
		size_t getCurrentData(size_t maxCount, double *tsBuf, T *dataBuf);
		size_t getDataCount();

		float getDataRate();

		size_t getBufferSize() const {
			return buferSize;
		}

};

template <class T>
DataBuffer<T>::DataBuffer(size_t bufferSize) {
	InitializeCriticalSection(&lock);
	this->bufferSize = bufferSize;
	data = (T *)malloc(bufferSize * sizeof(T));
	timestamps = (double *)malloc(bufferSize * sizeof(double));
	firstFree = firstUsed = count = 0;
}

template <class T>
DataBuffer<T>::~DataBuffer() {
	free(data);
	free(timestamps);
	DeleteCriticalSection(&lock);
}

template <class T>
void DataBuffer<T>::addData(double timestamp, T data) {
	EnterCriticalSection(&lock);
	this->timestamps[firstFree] = timestamp;
	this->data[firstFree] = data;
	firstFree = next(firstFree);
	count++;
	if (firstFree == firstUsed) {
		firstUsed = next(firstUsed);
		count--;
	}
	LeaveCriticalSection(&lock);
}

template <class T>
void DataBuffer<T>::getChunk(size_t start, size_t size, double *tsBuf, T *dataBuf) {
	if (start + size < bufferSize) {
		memcpy(tsBuf, timestamps + start, size * sizeof(double));
		memcpy(dataBuf, data + start, size * sizeof(T));
	} else {
		size_t first_half = bufferSize - start;
		size_t second_half = size - first_half;
		memcpy(tsBuf, timestamps + start, first_half * sizeof(double));
		memcpy(dataBuf, data + start, first_half * sizeof(T));
		memcpy(tsBuf + first_half, timestamps, second_half * sizeof(double));
		memcpy(dataBuf + first_half, data, second_half * sizeof(T));
	}
}

template <class T>
size_t DataBuffer<T>::getData(size_t maxCount, double *tsBuf, T *dataBuf) {
	EnterCriticalSection(&lock);
	size_t result_count = maxCount;
	if (result_count > count)
		result_count = count;
	if (result_count) {
		getChunk(firstUsed, result_count, tsBuf, dataBuf);
		firstUsed = (firstUsed + result_count) % bufferSize;
		count -= result_count;
	}
	LeaveCriticalSection(&lock);
	return result_count;
}

template <class T>
size_t DataBuffer<T>::getCurrentData(size_t maxCount, double *tsBuf, T *dataBuf) {
	EnterCriticalSection(&lock);
	size_t result_count = maxCount;
	if (result_count > count)
		result_count = count;
	if (result_count) {
		size_t first_return = (firstUsed + (count - result_count)) % bufferSize;
		getChunk(first_return, result_count, tsBuf, dataBuf);
	}
	LeaveCriticalSection(&lock);
	return result_count;
}

template <class T>
size_t DataBuffer<T>::getDataCount() {
	EnterCriticalSection(&lock);
	size_t result = this->count;
	LeaveCriticalSection(&lock);
	return result;
}

template <class T>
float DataBuffer<T>::getDataRate() {
	EnterCriticalSection(&lock);
	float result = 0.f;
	if (count > 1) {
		double first_ts = timestamps[firstUsed];
		double last_ts = timestamps[(firstUsed + count - 1) % bufferSize];
		return (float) (((double) (count-1)) / (last_ts - first_ts));
	}
	LeaveCriticalSection(&lock);
	return result;
}
