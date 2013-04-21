/*
 * RoboclawController.cpp
 *
 *  Created on: 30-11-2012
 *      Author: michal
 */

#include <log4cxx/propertyconfigurator.h>

#include "RoboclawController.h"
#include "RoboclawCommon.h"

#include <boost/program_options.hpp>
#include <string>
#include <cmath>

using namespace std;
using namespace boost;
using namespace boost::interprocess;
using namespace boost::program_options;
using namespace log4cxx;
using namespace amber;

LoggerPtr RoboclawController::_logger (Logger::getLogger("Roboclaw.Controller"));

RoboclawController::RoboclawController(int pipeInFd, int pipeOutFd, const char *confFilename) {

	parseConfigurationFile(confFilename);

	_roboclawDriver = new RoboclawDriver(_configuration);
	_amberPipes = new AmberPipes(this, pipeInFd, pipeOutFd);

	_roboclawDriver->initializeDriver();

	if (_configuration->battery_monitor_interval > 0) {
		_batteryMonitorThread = new boost::thread(boost::bind(&RoboclawController::batteryMonitor, this));	
	}
	
}

RoboclawController::~RoboclawController() {
	LOG4CXX_INFO(_logger, "Stopping controller.");
	 
	delete _roboclawDriver;
	delete _amberPipes;
}

void RoboclawController::handleDataMsg(amber::DriverHdr *driverHdr, amber::DriverMsg *driverMsg) {
	LOG4CXX_DEBUG(_logger, "Message came");

	// TODO: hack for now
	int clientId = driverHdr->clientids_size() > 0 ? driverHdr->clientids(0) : 0;

	// DataRequest
	if (driverMsg->HasExtension(roboclaw_proto::currentSpeedRequest)) {

		if (!driverMsg->has_synnum()) {
			LOG4CXX_WARN(_logger, "Got CurrentSpeedRequest, but syn num not set. Ignoring.");
			return;
		}

		if (driverMsg->GetExtension(roboclaw_proto::currentSpeedRequest)) {
			handleCurrentSpeedRequest(clientId, driverMsg->synnum());	
		}

	} else if (driverMsg->HasExtension(roboclaw_proto::motorsCommand)) {
		handleMotorsEncoderCommand(driverMsg->MutableExtension(roboclaw_proto::motorsCommand));
	}
}

void RoboclawController::handleClientDiedMsg(int clientID) {
	LOG4CXX_INFO(_logger, "Client " << clientID << " died");

	_roboclawDriver->stopMotors();
}

void RoboclawController::operator()() {
	_amberPipes->operator ()();
}

amber::DriverMsg *RoboclawController::buildCurrentSpeedMsg() {
	amber::DriverMsg *message = new amber::DriverMsg();
	message->set_type(amber::DriverMsg_MsgType_DATA);

	roboclaw_proto::MotorsSpeed *currentSpeed = message->MutableExtension(roboclaw_proto::currentSpeed);

	MotorsSpeedStruct mc;
	_roboclawDriver->readCurrentSpeed(&mc);

	currentSpeed->set_frontleftspeed(toMmps(mc.frontLeftSpeed));
	currentSpeed->set_frontrightspeed(toMmps(mc.frontRightSpeed));
	currentSpeed->set_rearleftspeed(toMmps(mc.rearLeftSpeed));
	currentSpeed->set_rearrightspeed(toMmps(mc.rearRightSpeed));

	return message;
}


void RoboclawController::sendCurrentSpeedMsg(int receiver, int ackNum) {
	LOG4CXX_DEBUG(_logger, "Sending currentSpeedRequest message");

	amber::DriverMsg *currentSpeedMsg = buildCurrentSpeedMsg();
	currentSpeedMsg->set_acknum(ackNum);
	amber::DriverHdr *header = new amber::DriverHdr();
	header->add_clientids(receiver);

	_amberPipes->writeMsgToPipe(header, currentSpeedMsg);

	delete currentSpeedMsg;
	delete header;
}


void RoboclawController::handleCurrentSpeedRequest(int sender, int synNum) {
	LOG4CXX_DEBUG(_logger, "Handling currentSpeedRequest message");

	sendCurrentSpeedMsg(sender, synNum);
}

void RoboclawController::handleMotorsEncoderCommand(roboclaw_proto::MotorsSpeed *motorsCommand) {
	LOG4CXX_DEBUG(_logger, "Handling motorsEncoderCommand message");
	MotorsSpeedStruct mc;

	mc.frontLeftSpeed = toQpps(motorsCommand->frontleftspeed());
	mc.frontRightSpeed = toQpps(motorsCommand->frontrightspeed());
	mc.rearLeftSpeed = toQpps(motorsCommand->rearleftspeed());
	mc.rearRightSpeed = toQpps(motorsCommand->rearrightspeed());

	_roboclawDriver->sendMotorsEncoderCommand(&mc);
}

int RoboclawController::toQpps(int in) {
	double rps = in / (double)(_configuration->wheel_radius * M_PI * 2);
	int out = (int)(rps * _configuration->pulses_per_revolution);

	LOG4CXX_DEBUG(_logger, "toOpps: " << in << ", " <<  out);

	return out;
}

int RoboclawController::toMmps(int in) {
	int out = (int)(in * (int)_configuration->wheel_radius * M_PI * 2 / (double)_configuration->pulses_per_revolution);
	
	LOG4CXX_DEBUG(_logger, "toMmps: " << in << ", " <<  out);

	return out;
}

void RoboclawController::batteryMonitor() {
	LOG4CXX_INFO(_logger, "Battery monitor thread started, interval: " << _configuration->battery_monitor_interval);

	__u16 voltage;

	while (1) {
		boost::this_thread::sleep(boost::posix_time::seconds(_configuration->battery_monitor_interval)); 
		_roboclawDriver->readMainBatteryVoltage(&voltage);
		
		LOG4CXX_INFO(_logger, "Main battery voltage level: " << voltage/10.0 << "V");
	}
	
}

void RoboclawController::parseConfigurationFile(const char *filename) {
	LOG4CXX_INFO(_logger, "Parsing configuration file: " << filename);

	_configuration = new RoboclawConfiguration();

	unsigned int front_rc_address;
	unsigned int rear_rc_address;

	options_description desc("Roboclaw options");
	desc.add_options()
			("roboclaw.uart_port", value<string>(&_configuration->uart_port)->default_value("/dev/ttyO3"))
			("roboclaw.uart_speed", value<unsigned int>(&_configuration->uart_speed)->default_value(38400))
			("roboclaw.front_rc_address", value<unsigned int>(&front_rc_address)->default_value(128))
			("roboclaw.rear_rc_address", value<unsigned int>(&rear_rc_address)->default_value(129))
			("roboclaw.motors_max_qpps", value<unsigned int>(&_configuration->motors_max_qpps)->default_value(13800))
			("roboclaw.motors_p_const", value<unsigned int>(&_configuration->motors_p_const)->default_value(65536))
			("roboclaw.motors_i_const", value<unsigned int>(&_configuration->motors_i_const)->default_value(32768))
			("roboclaw.motors_d_const", value<unsigned int>(&_configuration->motors_d_const)->default_value(16384))
			("roboclaw.pulses_per_revolution", value<unsigned int>(&_configuration->pulses_per_revolution)->default_value(1865))
			("roboclaw.wheel_radius", value<unsigned int>(&_configuration->wheel_radius)->default_value(60))
			("roboclaw.battery_monitor_interval", value<unsigned int>(&_configuration->battery_monitor_interval)->default_value(60));


	variables_map vm;

	try {
		store(parse_config_file<char>(filename, desc), vm);
		notify(vm);

		_configuration->front_rc_address = (__u8)front_rc_address;
		_configuration->rear_rc_address = (__u8)rear_rc_address;

	} catch (std::exception& e) {
		LOG4CXX_ERROR(_logger, "Error in parsing configuration file: " << e.what());
		exit(1);
	}

}

int main(int argc, char *argv[]) {

	if (argc < 3) {
		return 1;
	}

	const char *confFile = argv[1];
	const char *logConfFile = argv[2];

	PropertyConfigurator::configure(logConfFile);

	// STDIN_FD = 0, STDOUT_FD = 1
	// pipe_in_fd = 0, pipe_out_fd = 1
	LoggerPtr logger (Logger::getLogger("main"));

	LOG4CXX_INFO(logger, "-------------");	 
	LOG4CXX_INFO(logger, "Creating controller, config_file: " << argv[1] << ", log_config_file: " << argv[2]);

	RoboclawController controller(0, 1, confFile);
	controller();
}
