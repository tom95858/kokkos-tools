CXX=g++
CXXFLAGS=-O3 -std=c++11 -g -lcurl
SHARED_CXXFLAGS=-shared -fPIC

all: kp_kernel_timer.so ldms_upload

kp_kernel_timer.so: kp_kernel_timer.cpp kp_kernel_info.h ldms_upload.h ldms_upload.c
	$(CXX) $(SHARED_CXXFLAGS) $(CXXFLAGS) -o $@ kp_kernel_timer.cpp ldms_upload.c

ldms_upload: ldms_upload.c ldms_upload.h
	$(CXX) -DLDMS_UPLOAD_CMD=1 -O0 -g -lcurl -o $@ $<

clean:
	rm *.so ldms_upload
