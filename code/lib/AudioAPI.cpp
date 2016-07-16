/***********************************************************************
 * Author: Henrik Langer (henni19790@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ***********************************************************************/

#define PAD 0

#include "AudioAPI.hpp"
#include "WaveIO.hpp"
#define __CL_ENABLE_EXCEPTIONS
#include <CL/cl.hpp>
#include <ocl_util.h>
#include <functional>

void (*AudioAPI::_pCallbackFFT)(CallbackResponse *resData) = NULL;
void (*AudioAPI::_pCallbackIFFT)(CallbackResponse *resData) = NULL;

class AudioAPIImpl 
{
public:
    cl::Kernel *_fftKernel, *_ifftKernel;
    cl::Buffer *_bufFFTX, *_bufFFTY, *_bufFFTW;
    cl::Buffer *_bufIFFTX, *_bufIFFTY, *_bufIFFTW;
    cl::Program *_program;
    cl::CommandQueue *_Qfft, *_Qifft;
    cl::Context *_context;
};

void* AudioAPI::_allocBuffer(std::size_t size) {
    return __malloc_ddr(size);
}

/*
    Function for generating Specialized sequence of twiddle factors
*/
void AudioAPI::_twGenFFT(float *w, int n) {
    int i, j, k;
    const double PI = 3.141592654;

    for (j = 1, k = 0; j <= n >> 2; j = j << 2)
    {
        for (i = 0; i < n >> 2; i += j)
        {
            w[k]     = (float) sin (2 * PI * i / n);
            w[k + 1] = (float) cos (2 * PI * i / n);
            w[k + 2] = (float) sin (4 * PI * i / n);
            w[k + 3] = (float) cos (4 * PI * i / n);
            w[k + 4] = (float) sin (6 * PI * i / n);
            w[k + 5] = (float) cos (6 * PI * i / n);
            k += 6;
        }
    }
}

void AudioAPI::_twGenIFFT(float *w, int n) {
    int i, j, k;
    const double PI = 3.141592654;

    for (j = 1, k = 0; j <= n >> 2; j = j << 2)
    {
        for (i = 0; i < n >> 2; i += j)
        {
            w[k]     = (float) (-1)*sin (2 * PI * i / n);
            w[k + 1] = (float) cos (2 * PI * i / n);
            w[k + 2] = (float) (-1)*sin (4 * PI * i / n);
            w[k + 3] = (float) cos (4 * PI * i / n);
            w[k + 4] = (float) (-1)*sin (6 * PI * i / n);
            w[k + 5] = (float) cos (6 * PI * i / n);
            k += 6;
        }
    }
}

AudioAPI::AudioAPI() : _N_fft(-1), _N_ifft(-1), _pimpl(new AudioAPIImpl()) {
	try {
        _pimpl->_context = new cl::Context(CL_DEVICE_TYPE_ACCELERATOR);
        std::vector<cl::Device> devices = _pimpl->_context->getInfo<CL_CONTEXT_DEVICES>();

        int num;
 		devices[0].getInfo(CL_DEVICE_MAX_COMPUTE_UNITS, &num);
 		std::cout << "Found " << num << " DSP compute cores." << std::endl;

        std::ifstream t("audiokernel.cl");
        if (!t) { std::cerr << "Error Opening Kernel Source file\n"; exit(-1); }

        std::string kSrc((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        cl::Program::Sources source(1, std::make_pair(kSrc.c_str(),kSrc.length()));
        _pimpl->_program = new cl::Program(*_pimpl->_context, source);
        _pimpl->_program->build(devices, "./dsplib.ae66");

        _pimpl->_Qfft = new cl::CommandQueue(*_pimpl->_context, devices[0], CL_QUEUE_PROFILING_ENABLE);
        _pimpl->_Qifft = new cl::CommandQueue(*_pimpl->_context, devices[0], CL_QUEUE_PROFILING_ENABLE);
    }
    catch(cl::Error &err) {
        std::cerr << "ERROR: " << err.what() << "(" << err.err() << ")" << std::endl;
    }
}

AudioAPI::~AudioAPI() {
	delete _pimpl->_bufFFTX;
	delete _pimpl->_bufFFTY;
	delete _pimpl->_bufFFTW;
	delete _pimpl->_program;
	if (_wFFT)
        __free_ddr(_wFFT);
    if (_xFFT)
        __free_ddr(_xFFT);
    if (_yFFT)
        __free_ddr(_yFFT);
}

int AudioAPI::ocl_DSPF_sp_fftSPxSP(int N, float *x,
		float *y, int n_min, int n_max,
		void (*callback)(CallbackResponse *resData)) {

    try{
        if (N != _N_fft) {
            _N_fft = N;
            /*delete _fftKernel;
            delete _bufFFTX;
            delete _bufFFTY;
            delete _bufFFTW;

            if (_wFFT)
                __free_ddr(_wFFT);
            if (_xFFT)
                __free_ddr(_xFFT);
            if (_yFFT)
                __free_ddr(_yFFT);*/

            _bufsize_fft = sizeof(float) * (2*_N_fft + PAD + PAD);
            _wFFT = (float*) _allocBuffer(sizeof(float)*2*_N_fft);
            _xFFT = (float*) _allocBuffer(sizeof(float)*2*_N_fft);
            _yFFT = (float*) _allocBuffer(sizeof(float)*2*_N_fft);

            _twGenFFT(_wFFT, _N_fft);

            _pimpl->_bufFFTX = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,  _bufsize_fft, _xFFT);
            _pimpl->_bufFFTY = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, _bufsize_fft, _yFFT);
            _pimpl->_bufFFTW = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,  _bufsize_fft, _wFFT);

            _pimpl->_fftKernel = new cl::Kernel(*_pimpl->_program, "ocl_DSPF_sp_fftSPxSP");
            _pimpl->_fftKernel->setArg(0, _N_fft);
            _pimpl->_fftKernel->setArg(1, *_pimpl->_bufFFTX);
            _pimpl->_fftKernel->setArg(2, *_pimpl->_bufFFTW);
            _pimpl->_fftKernel->setArg(3, *_pimpl->_bufFFTY);
            int rad = 4;
            _pimpl->_fftKernel->setArg(4, rad); //n_min
            _pimpl->_fftKernel->setArg(5, _N_fft); //n_max
        }

        for (int i=0; i < _N_fft; i++)
            _xFFT[i] = x[i];

        cl::Event ev1;
        std::vector<cl::Event> evs(2);
        std::vector<cl::Event> evss(1);
        _pimpl->_Qfft->enqueueWriteBuffer(*_pimpl->_bufFFTX, CL_FALSE, 0, _bufsize_fft, x, 0, &evs[0]);
        _pimpl->_Qfft->enqueueWriteBuffer(*_pimpl->_bufFFTW, CL_FALSE, 0, _bufsize_fft, _wFFT, 0, &evs[1]);
        _pimpl->_Qfft->enqueueNDRangeKernel(*_pimpl->_fftKernel, cl::NullRange, cl::NDRange(1), cl::NDRange(1), &evs, &evss[0]);
        _pimpl->_Qfft->enqueueReadBuffer(*_pimpl->_bufFFTY, CL_TRUE, 0, _bufsize_fft, y, &evss, &ev1);

        _pCallbackFFT = callback;
        auto lambda = [](cl_event ev, cl_int e_status, void *user_data) {
            CallbackResponse *res = (CallbackResponse*) user_data;
            _pCallbackFFT(res);
        };

        CallbackResponse *clbkRes = new CallbackResponse(CallbackResponse::FFT, 2*_N_fft, y);
        ev1.setCallback(CL_COMPLETE, lambda, clbkRes);

        //ocl_event_times(evs[0], "Write X");
        //ocl_event_times(evs[1], "Twiddle");
        ocl_event_times(ev1, "FFT");
        //ocl_event_times(ev2, "Read Y");

        //TODO: Generate and return ID for referencing task in callback
    }
    catch (cl::Error &err)
    { 
        std::cerr << "ERROR: " << err.what() << "(" << err.err() << ")" << std::endl; 
    }

    return 0;
}

int AudioAPI::ocl_DSPF_sp_ifftSPxSP(int N, float *x,
	float *y, int n_min, int n_max,
	void (*callback)(CallbackResponse *resData)) {

	try{
		if (N != _N_ifft) {
			_N_ifft = N;
	        /*delete _ifftKernel;
	        delete _bufIFFTX;
	        delete _bufIFFTY;
	        delete _bufIFFTW;

	        if (_wIFFT)
	            __free_ddr(_wIFFT);
            if (_xIFFT)
                __free_ddr(_xIFFT);
            if (_yIFFT)
                __free_ddr(_yIFFT);*/

	        _bufsize_ifft = sizeof(float) * (2*_N_ifft + PAD + PAD);
	        _wIFFT = (float*) _allocBuffer(sizeof(float)*2*_N_ifft);
            _xIFFT = (float*) _allocBuffer(sizeof(float)*2*_N_ifft);
            _yIFFT = (float*) _allocBuffer(sizeof(float)*2*_N_ifft);

	        _twGenIFFT(_wIFFT, _N_ifft);

	        _pimpl->_bufIFFTX = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,  _bufsize_ifft, _xIFFT);
	        _pimpl->_bufIFFTY = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, _bufsize_ifft, _yIFFT);
	        _pimpl->_bufIFFTW = new cl::Buffer(*_pimpl->_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,  _bufsize_ifft, _wIFFT);

	        _pimpl->_ifftKernel = new cl::Kernel(*_pimpl->_program, "ocl_DSPF_sp_ifftSPxSP");
	        _pimpl->_ifftKernel->setArg(0, _N_ifft);
	        _pimpl->_ifftKernel->setArg(1, *_pimpl->_bufIFFTX);
	        _pimpl->_ifftKernel->setArg(2, *_pimpl->_bufIFFTW);
	        _pimpl->_ifftKernel->setArg(3, *_pimpl->_bufIFFTY);
	        int rad = 4;
	        _pimpl->_ifftKernel->setArg(4, rad); //n_min
	        _pimpl->_ifftKernel->setArg(5, _N_ifft); //n_max
	    }

        for (int i=0; i < _N_ifft; i++)
            _xIFFT[i] = x[i];

	    cl::Event ev1;
	    std::vector<cl::Event> evs(2);
	    std::vector<cl::Event> evss(1);
	    _pimpl->_Qifft->enqueueWriteBuffer(*_pimpl->_bufIFFTX, CL_FALSE, 0, _bufsize_ifft, x, 0, &evs[0]);
	    _pimpl->_Qifft->enqueueWriteBuffer(*_pimpl->_bufIFFTW, CL_FALSE, 0, _bufsize_ifft, _wIFFT, 0, &evs[1]);
	    _pimpl->_Qifft->enqueueNDRangeKernel(*_pimpl->_ifftKernel, cl::NullRange, cl::NDRange(1), cl::NDRange(1), &evs, &evss[0]);
	    _pimpl->_Qifft->enqueueReadBuffer(*_pimpl->_bufIFFTY, CL_TRUE, 0, _bufsize_ifft, y, &evss, &ev1);

        _pCallbackIFFT = callback;
        auto lambda = [](cl_event ev, cl_int e_status, void *user_data) {
            CallbackResponse *res = (CallbackResponse*) user_data;
            _pCallbackIFFT(res);
        };

	    CallbackResponse *clbkRes = new CallbackResponse(CallbackResponse::IFFT, 2*_N_ifft, y);
	    ev1.setCallback(CL_COMPLETE, lambda, clbkRes);

	    //ocl_event_times(evs[0], "Write X");
	    //ocl_event_times(evs[1], "Twiddle");
	    ocl_event_times(ev1, "IFFT");
	    //ocl_event_times(ev2, "Read Y");

	    //TODO: Generate and return ID for referencing task in callback
	}
	catch (cl::Error &err)
	{ 
        std::cerr << "ERROR: " << err.what() << "(" << err.err() << ")" << std::endl;
    }

    return 0;
}

/*int AudioAPI::convReverbFromWAV(int N, float *x, const std::string &filename, float *y,
        void (*callback)(cl_event ev, cl_int e_status, void *user_data)){
    try {
        return 0;
    }
    catch (cl::Error &err)
    { std::cerr << "ERROR: " << err.what() << "(" << err.err() << ")" << std::endl; }
}*/
