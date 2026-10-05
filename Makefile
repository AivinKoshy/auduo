export PATH := $(CURDIR)/tools/w64devkit/bin;$(PATH)
CXX = tools/w64devkit/bin/g++
WINDRES = windres
SHELL = cmd.exe
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -DUNICODE -D_UNICODE \
           -Iinclude -Ithird_party/imgui -Ithird_party/imgui/backends

LDFLAGS = -static -static-libgcc -static-libstdc++ -mwindows \
          -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lole32 -loleaut32 -lmmdevapi -luuid -lavrt -lksuser -lwinmm

SRC_APP = src/main.cpp src/AudioEngine.cpp src/RingBuffer.cpp src/UI.cpp
SRC_IMGUI = third_party/imgui/imgui.cpp \
            third_party/imgui/imgui_draw.cpp \
            third_party/imgui/imgui_tables.cpp \
            third_party/imgui/imgui_widgets.cpp \
            third_party/imgui/backends/imgui_impl_win32.cpp \
            third_party/imgui/backends/imgui_impl_dx11.cpp

OBJ = build/main.o build/AudioEngine.o build/RingBuffer.o build/UI.o \
      build/imgui.o build/imgui_draw.o build/imgui_tables.o build/imgui_widgets.o \
      build/backend_win32.o build/backend_dx11.o build/resource.o

TARGET = bin/auduo.exe

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@
	@echo Build Complete: $(TARGET)

build/resource.o: resources/auduo.rc resources/auduo.exe.manifest
	$(WINDRES) -I. -Iresources resources/auduo.rc -o $@

build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/imgui.o: third_party/imgui/imgui.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/imgui_draw.o: third_party/imgui/imgui_draw.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/imgui_tables.o: third_party/imgui/imgui_tables.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/imgui_widgets.o: third_party/imgui/imgui_widgets.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/backend_win32.o: third_party/imgui/backends/imgui_impl_win32.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/backend_dx11.o: third_party/imgui/backends/imgui_impl_dx11.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	del /Q build\*.o bin\*.exe
