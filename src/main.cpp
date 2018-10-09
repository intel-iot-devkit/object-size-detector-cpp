/*
* Copyright (c) 2018 Intel Corporation.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
* LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
* OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

// std includes
#include <iostream>
#include <stdio.h>
#include <thread>
#include <queue>
#include <map>
#include <atomic>
#include <csignal>
#include <ctime>
#include <mutex>
#include <syslog.h>
#include <string>

// OpenCV includes
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// MQTT
#include "mqtt.h"

using namespace std;
using namespace cv;
using namespace dnn;

// OpenCV-related variables
Mat frame, blob;
VideoCapture cap;
int delay = 5;
int rate;
// nextImage provides queue for captured video frames
queue<Mat> nextImage;

// flag to control background threads
atomic<bool> keepRunning(true);

// flag to handle UNIX signals
static volatile sig_atomic_t sig_caught = 0;

// mqtt parameters
const string topic = "defects/counter";

// assembly part and defect areas
int min_area;
int max_area;

// AssemblyInfo contains information about assembly line defects
struct AssemblyInfo
{
    bool defect;
};

// currentInfo contains the latest AssemblyInfo as tracked by the application
AssemblyInfo currentInfo = {false};

mutex m, m1, m2;

const char* keys =
    "{ help h      | | Print help message. }"
    "{ device d    | 0 | camera device number. }"
    "{ input i     | | Path to input image or video file. Skip this argument to capture frames from a camera. }"
    "{ minarea min | 10000 | Minimum part area of assembly object. }"
    "{ maxarea max | 30000 | Maximum part area of assembly object. }"
    "{ backend b    | 0 | Choose one of computation backends: "
                        "0: automatically (by default), "
                        "1: Halide language (http://halide-lang.org/), "
                        "2: Intel's Deep Learning Inference Engine (https://software.intel.com/openvino-toolkit), "
                        "3: OpenCV implementation }"
    "{ target t     | 0 | Choose one of target computation devices: "
                        "0: CPU target (by default), "
                        "1: OpenCL, "
                        "2: OpenCL fp16 (half-float precision), "
                        "3: VPU }"
    "{ rate r      | 1 | number of seconds between data updates to MQTT server. }";

// nextImageAvailable returns the next image from the queue in a thread-safe way
Mat nextImageAvailable() {
    Mat rtn;
    m.lock();
    if (!nextImage.empty()) {
        rtn = nextImage.front();
        nextImage.pop();
    }
    m.unlock();

    return rtn;
}

// addImage adds an image to the queue in a thread-safe way
void addImage(Mat img) {
    m.lock();
    if (nextImage.empty()) {
        nextImage.push(img);
    }
    m.unlock();
}

// getCurrentInfo returns the most-recent AssemblyInfo for the application.
AssemblyInfo getCurrentInfo() {
    m2.lock();
    AssemblyInfo info;
    info = currentInfo;
    m2.unlock();

    return info;
}

// updateInfo uppdates the current AssemblyInfo for the application to the latest detected values
void updateInfo(AssemblyInfo info) {
    m2.lock();
    currentInfo.defect = info.defect;
    m2.unlock();
}

// resetInfo resets the current AssemblyInfo for the application.
void resetInfo() {
    m2.lock();
    currentInfo.defect = false;
    m2.unlock();
}

// publish MQTT message with a JSON payload
void publishMQTTMessage(const string& topic, const AssemblyInfo& info)
{
    ostringstream s;
    s << "{\"Defect\": \"" << info.defect << "\"}";
    string payload = s.str();

    mqtt_publish(topic, payload);

    string msg = "MQTT message published to topic: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());
    syslog(LOG_INFO, "%s", payload.c_str());
}

// message handler for the MQTT subscription for the any desired control channel topic
int handleMQTTControlMessages(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    string topic = topicName;
    string msg = "MQTT message received: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());

    return 1;
}

// Function called by worker thread to process the next available video frame.
void frameRunner() {
    while (keepRunning.load()) {
        Mat next = nextImageAvailable();
        if (!next.empty()) {
            Mat img;
            int max_blob_area = 0;
            int part_area = 0;
            bool defect = false;
            Size size(3,3);
            vector<Vec4i> hierarchy;
            vector<vector<Point> > contours;

            cvtColor(frame, img, CV_RGB2GRAY);
            GaussianBlur(img, img, size, 0, 0 );

            morphologyEx(img, img, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, size));
            morphologyEx(img, img, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, size));
            morphologyEx(img, img, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, size));

            threshold(img, img, 200, 255, THRESH_BINARY);

            findContours(img, contours, hierarchy, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_NONE);

            for (size_t i = 0; i < contours.size(); i++)
            {
                Rect r = boundingRect(contours[i]);
                rectangle(frame, r, Scalar(0, 0, 255));
                part_area = r.width * r.height;
                if (part_area > max_blob_area)
                {
                    max_blob_area = part_area;
                }
            }
            part_area = max_blob_area;

            if (part_area != 0) {
                if (part_area > max_area || part_area < min_area)
                {
                    defect = true;
                }
            }

            AssemblyInfo info;
            info.defect = defect;

            updateInfo(info);
        }
    }

    cout << "Video processing thread stopped" << endl;
}

// Function called by worker thread to handle MQTT updates. Pauses for rate second(s) between updates.
void messageRunner() {
    while (keepRunning.load()) {
        AssemblyInfo info = getCurrentInfo();
        publishMQTTMessage(topic, info);
        this_thread::sleep_for(chrono::seconds(rate));
    }

    cout << "MQTT sender thread stopped" << endl;
}

// signal handler for the main thread
void handle_sigterm(int signum)
{
    /* we only handle SIGTERM and SIGKILL here */
    if (signum == SIGTERM) {
        cout << "Interrupt signal (" << signum << ") received" << endl;
        sig_caught = 1;
    }
}

int main(int argc, char** argv)
{
    // parse command parameters
    CommandLineParser parser(argc, argv, keys);
    parser.about("Use this script to using OpenVINO.");
    if (argc == 1 || parser.has("help"))
    {
        parser.printMessage();

        return 0;
    }

    min_area = parser.get<int>("minarea");
    max_area = parser.get<int>("maxarea");

    cout << "Min Area: " << min_area << endl;
    cout << "Max Area: " << max_area << endl;

    // connect MQTT messaging
    int result = mqtt_start(handleMQTTControlMessages);
    if (result == 0) {
        syslog(LOG_INFO, "MQTT started.");
    } else {
        syslog(LOG_INFO, "MQTT NOT started: have you set the ENV varables?");
    }

    mqtt_connect();

    // open video capture source
    if (parser.has("input")) {
        cap.open(parser.get<String>("input"));

        // also adjust delay so video playback matches the number of FPS in the file
        double fps = cap.get(CAP_PROP_FPS);
        delay = 1000/fps;
    }
    else
        cap.open(parser.get<int>("device"));

    if (!cap.isOpened()) {
        cerr << "ERROR! Unable to open video source\n";
        return -1;
    }

    // register SIGTERM signal handler
    signal(SIGTERM, handle_sigterm);

    // start worker threads
    thread t1(frameRunner);
    //thread t2(messageRunner);

    string label;
    // read video input data
    for (;;) {
        cap.read(frame);

        if (frame.empty()) {
            keepRunning = false;
            cerr << "ERROR! blank frame grabbed\n";
            break;
        }

        resize(frame, frame, Size(960, 540));
        addImage(frame);

        AssemblyInfo info = getCurrentInfo();
        label = format("Defect: %d", info.defect);
        putText(frame, label, Point(0, 40), FONT_HERSHEY_SIMPLEX, 0.5, CV_RGB(255, 0, 0));

        imshow("Assembly Line Measurements", frame);

        if (waitKey(delay) >= 0 || sig_caught) {
            cout << "Attempting to stop background threads" << endl;
            keepRunning = false;
            break;
        }
    }

    // wait for the threads to finish
    t1.join();
    t2.join();
    cap.release();

    // disconnect MQTT messaging
    mqtt_disconnect();
    mqtt_close();

    return 0;
}
