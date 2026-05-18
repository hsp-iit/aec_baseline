/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include <yarp/os/Network.h>
#include <yarp/os/LogStream.h>
#include <iostream>
#include <string>
#include "AECComponent.hpp"

int main(int argc, char *argv[])
{
    // Initialize YARP network
    yarp::os::Network yarp;

    if (!yarp.checkNetwork())
    {
        yError() << "YARP network is not available!";
        return EXIT_FAILURE;
    }

    // Create and configure the AEC component
    AECComponent aecComponent;

    yarp::os::ResourceFinder rf;
    rf.setDefaultConfigFile("../aec_config.ini");
    rf.setDefaultContext("aec");
    rf.configure(argc, argv);

    if (!aecComponent.configure(rf))
    {
        yError() << "Failed to configure AECComponent";
        return EXIT_FAILURE;
    }

    yInfo() << "AEC Component started successfully";
    yInfo() << "Type 'quit' to exit";

    std::string command;
    while (std::getline(std::cin, command))
    {
        if (command == "quit")
        {
            break;
        }
    }

    aecComponent.close();

    yInfo() << "AEC Component stopped";
    return EXIT_SUCCESS;
}
