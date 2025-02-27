#include <algorithm>
#include <set>
#include <vector>
#include <wiringPi.h>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "misc.h"

std::vector<std::string> transfer_types{"control", "int", "bulk", "isoc"};
std::vector<std::string> control_type{"modify", "ignore", "stall"};
std::string transfer_type = "control";

std::set<unsigned int> used_gpio_pins;
std::map<unsigned char, struct usb_raw_transfer_io> last_messages;

enum class RuleType {
	Default = 0,
	RaspberryPiGpio = 1
};

enum class ByteReplacementType {
	Replace = 0,
	BitwiseOr = 1
};

bool is_any_gpio_pin_triggered() 
{
	return std::any_of(used_gpio_pins.begin(), used_gpio_pins.end(), [](unsigned int gpio_index){ 
		return (digitalRead(gpio_index) == LOW); 
	});
}

void injection(struct usb_raw_transfer_io &io, Json::Value patterns, std::string replacement_hex, bool &data_modified) {
	std::string data(io.data, io.inner.length);
	std::string replacement = hexToAscii(replacement_hex);
	for (unsigned int j = 0; j < patterns.size(); j++) {
		std::string pattern_hex = patterns[j].asString();
		std::string pattern = hexToAscii(pattern_hex);

		std::string::size_type pos = data.find(pattern);
		while (pos != std::string::npos) {
			if (data.length() - pattern.length() + replacement.length() > 1023)
				break;

			data = data.replace(pos, pattern.length(), replacement);
			printf("Modified from %s to %s at Index %ld\n", pattern_hex.c_str(), replacement_hex.c_str(), pos);
			data_modified = true;

			pos = data.find(pattern);
		}
	}

	if (data_modified) {
		io.inner.length = data.length();
		for (size_t j = 0; j < data.length(); j++) {
			io.data[j] = data[j];
		}
	}
}

void injection(struct usb_raw_control_event &event, struct usb_raw_transfer_io &io, int &injection_flags) {
	// This is just a simple injection function for control transfer.
	for (unsigned int i = 0; i < control_type.size(); i++) {
		for (unsigned int j = 0; j < injection_config[transfer_type][control_type[i]].size(); j++) {
			Json::Value rule = injection_config[transfer_type][control_type[i]][j];
			if (rule["enable"].asBool() != true)
				continue;

			if (event.ctrl.bRequestType != hexToDecimal(rule["bRequestType"].asInt()) ||
			    event.ctrl.bRequest     != hexToDecimal(rule["bRequest"].asInt()) ||
			    event.ctrl.wValue       != hexToDecimal(rule["wValue"].asInt()) ||
			    event.ctrl.wIndex       != hexToDecimal(rule["wIndex"].asInt()) ||
			    event.ctrl.wLength      != hexToDecimal(rule["wLength"].asInt()))
				continue;

			printf("Matched injection rule: %s, index: %d\n", control_type[i].c_str(), j);
			if (control_type[i] == "modify") {
				Json::Value patterns = rule["content_pattern"];
				std::string replacement_hex = rule["replacement"].asString();
				bool data_modified = false;

				injection(io, patterns, replacement_hex, data_modified);
				if (!(event.ctrl.bRequestType & USB_DIR_IN))
					event.ctrl.wLength = io.inner.length;
			}
			else if (control_type[i] == "ignore") {
				printf("Ignore this control transfer\n");
				injection_flags = USB_INJECTION_FLAG_IGNORE;
			}
			else if (control_type[i] == "stall") {
				injection_flags = USB_INJECTION_FLAG_STALL;
			}
		}
	}
}

void injection(struct usb_raw_transfer_io &io, struct usb_endpoint_descriptor ep, std::string transfer_type) {
	// This is just a simple injection function for int and bulk transfer.
	for (unsigned int i = 0; i < injection_config[transfer_type].size(); i++) {
		Json::Value rule = injection_config[transfer_type][i];
		if (rule["enable"].asBool() != true ||
		    hexToDecimal(rule["ep_address"].asInt()) != ep.bEndpointAddress)
			continue;

		// Backwards compatibility: "type" might not exist, use default if "type" is missing.
		RuleType rule_type = RuleType::Default;
		if(rule.isMember("type"))
		{
			rule_type = static_cast<RuleType>(rule["type"].asUInt());
		}

		if(rule_type == RuleType::Default)
		{
			Json::Value patterns = rule["content_pattern"];
			std::string replacement_hex = rule["replacement"].asString();
			bool data_modified = false;

			injection(io, patterns, replacement_hex, data_modified);

			if (data_modified)
				break;
		}
		else if(rule_type == RuleType::RaspberryPiGpio)
		{
			auto gpio_on = rule["gpio"]["on"];
			const bool are_all_required_on = std::all_of(gpio_on.begin(), gpio_on.end(), [](Json::Value gpio_index){
				return (digitalRead(gpio_index.asUInt()) == LOW);
			});

			auto gpio_off = rule["gpio"]["off"];
			const bool are_all_required_off = std::all_of(gpio_off.begin(), gpio_off.end(), [](Json::Value gpio_index){
				return (digitalRead(gpio_index.asUInt()) != LOW);
			});

			bool is_condition_met = are_all_required_on && are_all_required_off;
			if(!is_condition_met)
			{
				continue;
			}

			// Consider "replace" type as default, if it is not set.
			ByteReplacementType replacement_type = ByteReplacementType::Replace;
			if(rule.isMember("byte_replacement_type"))
			{
				replacement_type = static_cast<ByteReplacementType>(rule["byte_replacement_type"].asUInt());
			}

			for (unsigned int j = 0; j < rule["byte_replacements"].size(); j++) {
				Json::Value byte_replacement_rule = rule["byte_replacements"][j];

				unsigned int index = byte_replacement_rule["index"].asUInt();
				unsigned int value = byte_replacement_rule["value"].asUInt();

				if(index >= io.inner.length)
				{
					continue;
				}

				//printf("GPIO %s signal detected, modifying byte %d\n", rule["gpio"].asString().c_str(), index);

				switch(replacement_type) {
				case ByteReplacementType::Replace: {
					io.data[index] = char(value);
					break;
				}
				case ByteReplacementType::BitwiseOr: {
					io.data[index] = io.data[index] | char(value);
					break;
				}
				}
			}
		}
	}
}

void printData(struct usb_raw_transfer_io io, __u8 bEndpointAddress, std::string transfer_type, std::string dir) {
	printf("Sending data to EP%x(%s_%s):", bEndpointAddress,
		transfer_type.c_str(), dir.c_str());
	for (unsigned int i = 0; i < io.inner.length; i++) {
		printf(" %02hhx", (unsigned)io.data[i]);
	}
	printf("\n");
}

void *ep_loop_write(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;

	printf("Start writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	while (!please_stop_eps) {
		assert(ep_num != -1);
		if (data_queue->size() == 0) {
			usleep(100);
			continue;
		}

		data_mutex->lock();
		struct usb_raw_transfer_io io = data_queue->front();
		data_queue->pop_front();
		data_mutex->unlock();

		if (verbose_level >= 2)
			printData(io, ep.bEndpointAddress, transfer_type, dir);

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv > 0) {
				printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
			}
		}
		else {
			int length = io.inner.length;
			unsigned char *data = new unsigned char[length];
			memcpy(data, io.data, length);
			send_data(ep.bEndpointAddress, ep.bmAttributes, data, length);

			if (data)
				delete[] data;
		}
	}

	printf("End writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void *ep_loop_read(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	while (!please_stop_eps) {
		assert(ep_num != -1);
		struct usb_raw_transfer_io io;
		
		if (ep.bEndpointAddress & USB_DIR_IN) {
			unsigned char *data = NULL;
			int nbytes = -1;

			if (data_queue->size() >= 32) {
				printf("EP%x(%s_%s): queue contains %lu, sleeping\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), data_queue->size());
				usleep(100);
				continue;
			}

			receive_data(ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize, &data, &nbytes, 20);

			if (nbytes > 0) {
				memcpy(io.data, data, nbytes);
				io.inner.ep = ep_num;
				io.inner.flags = 0;
				io.inner.length = nbytes;

				if (injection_enabled)
					injection(io, ep, transfer_type);

				data_mutex->lock();
				data_queue->push_back(io);
				last_messages[ep.bEndpointAddress] = io;
				data_mutex->unlock();

				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), nbytes);
			}

			if(is_any_gpio_pin_triggered() && last_messages.count(ep.bEndpointAddress) > 0)
			{
				struct usb_raw_transfer_io last_io = last_messages[ep.bEndpointAddress];

				if (injection_enabled)
					injection(last_io, ep, transfer_type);

				data_mutex->lock();
				data_queue->push_back(last_io);
				data_mutex->unlock();

				if (verbose_level)
					printf("EP%x(%s_%s): artificially enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), last_io.inner.length);
			}

			if (data)
				delete[] data;
		}
		else 
		{
			io.inner.ep = ep_num;
			io.inner.flags = 0;
			io.inner.length = sizeof(io.data);

			int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
			if (rv >= 0) {
				printf("EP%x(%s_%s): read %d bytes from host\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
				io.inner.length = rv;

				if (injection_enabled)
					injection(io, ep, transfer_type);

				data_mutex->lock();
				data_queue->push_back(io);
				last_messages[ep.bEndpointAddress] = io;
				data_mutex->unlock();

				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), rv);
			}
		}
	}

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void process_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	printf("Activating %d endpoints on interface %d\n", (int)alt->interface.bNumEndpoints, interface);

	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		int addr = usb_endpoint_num(&ep->endpoint);
		assert(addr != 0);

		ep->thread_info.fd = fd;
		ep->thread_info.endpoint = ep->endpoint;
		ep->thread_info.data_queue = new std::deque<usb_raw_transfer_io>;
		ep->thread_info.data_mutex = new std::mutex;

		switch (usb_endpoint_type(&ep->endpoint)) {
		case USB_ENDPOINT_XFER_ISOC:
			ep->thread_info.transfer_type = "isoc";
			break;
		case USB_ENDPOINT_XFER_BULK:
			ep->thread_info.transfer_type = "bulk";
			break;
		case USB_ENDPOINT_XFER_INT:
			ep->thread_info.transfer_type = "int";
			break;
		default:
			printf("transfer_type %d is invalid\n", usb_endpoint_type(&ep->endpoint));
			assert(false);
		}

		if (usb_endpoint_dir_in(&ep->endpoint))
			ep->thread_info.dir = "in";
		else
			ep->thread_info.dir = "out";

		ep->thread_info.ep_num = usb_raw_ep_enable(fd, &ep->thread_info.endpoint);
		printf("%s_%s: addr = %u, ep = #%d\n",
			ep->thread_info.transfer_type.c_str(),
			ep->thread_info.dir.c_str(),
			addr, ep->thread_info.ep_num);

		if (verbose_level)
			printf("Creating thread for EP%02x\n",
				ep->thread_info.endpoint.bEndpointAddress);
		pthread_create(&ep->thread_read, 0,
			ep_loop_read, (void *)&ep->thread_info);
		pthread_create(&ep->thread_write, 0,
			ep_loop_write, (void *)&ep->thread_info);
	}
   
	printf("Activating wiringPi API\n");

	wiringPiSetupGpio();

	for (unsigned int j = 0; j < injection_config["int"].size(); j++) {
		Json::Value rule = injection_config["int"][j];
		if (rule["enable"].asBool() != true)
			continue;

		const RuleType injection_type = static_cast<RuleType>(rule["type"].asUInt());
		if (injection_type != RuleType::RaspberryPiGpio)
			continue;

		auto gpio_on = rule["gpio"]["on"];
		std::for_each(gpio_on.begin(), gpio_on.end(), [](Json::Value gpio_index){
			used_gpio_pins.insert(gpio_index.asUInt());
		});

		auto gpio_off = rule["gpio"]["off"];
		std::for_each(gpio_off.begin(), gpio_off.end(), [](Json::Value gpio_index){
			used_gpio_pins.insert(gpio_index.asUInt());
		});
	}

	std::for_each(used_gpio_pins.begin(), used_gpio_pins.end(), [](unsigned int gpio_index){ 
		pinMode(gpio_index, INPUT);
		pullUpDnControl(gpio_index, PUD_UP); 
		printf("wiringPi: activated pin %d as input\n", gpio_index);
	});

	printf("process_eps done\n");
}

void terminate_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	please_stop_eps = true;

	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		if (ep->thread_read && pthread_join(ep->thread_read, NULL)) {
			fprintf(stderr, "Error join thread_read\n");
		}
		if (ep->thread_write && pthread_join(ep->thread_write, NULL)) {
			fprintf(stderr, "Error join thread_write\n");
		}
		ep->thread_read = 0;
		ep->thread_write = 0;

		usb_raw_ep_disable(fd, ep->thread_info.ep_num);
		ep->thread_info.ep_num = -1;

		delete ep->thread_info.data_queue;
		delete ep->thread_info.data_mutex;
	}

	please_stop_eps = false;
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;

	printf("Start for EP0, thread id(%d)\n", gettid());

	if (verbose_level)
		print_eps_info(fd);

	while (!please_stop_ep0) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);

		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);

		if (event.inner.length == 4294967295) {
			printf("End for EP0, thread id(%d)\n", gettid());
			return;
		}

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_transfer_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = event.ctrl.wLength;

		int injection_flags = USB_INJECTION_FLAG_NONE;
		int nbytes = 0;
		int result = 0;
		unsigned char *control_data = new unsigned char[event.ctrl.wLength];

		int rv = -1;
		if (event.ctrl.bRequestType & USB_DIR_IN) {
			result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
			if (result == 0) {
				memcpy(&io.data[0], control_data, nbytes);
				io.inner.length = nbytes;

				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "in");

				rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
				printf("ep0: transferred %d bytes (in)\n", rv);
			}
			else {
				usb_raw_ep0_stall(fd);
			}
		}
		else {
			rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);

			if (event.ctrl.bRequestType == 0x00 && event.ctrl.bRequest == 0x09) { // Set configuration
				int desired_config = -1;
				for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
					if (host_device_desc.configs[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_config = i;
						break;
					}
				}
				if (desired_config < 0) {
					printf("[Warning] Skip changing configuration, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				struct raw_gadget_config *config = &host_device_desc.configs[desired_config];

				if (set_configuration_done_once) { // Need to stop all threads for eps and cleanup
					printf("Changing configuration\n");
					for (int i = 0; i < config->config.bNumInterfaces; i++) {
						struct raw_gadget_interface *iface = &config->interfaces[i];
						int interface_num = iface->altsettings[iface->current_altsetting]
							.interface.bInterfaceNumber;
						terminate_eps(fd, host_device_desc.current_config, i,
								iface->current_altsetting);
						release_interface(interface_num);
					}
				}

				usb_raw_configure(fd);
				set_configuration(config->config.bConfigurationValue);
				host_device_desc.current_config = desired_config;

				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					iface->current_altsetting = 0;
					int interface_num = iface->altsettings[0].interface.bInterfaceNumber;
					claim_interface(interface_num);
					process_eps(fd, desired_config, i, 0);
				}

				set_configuration_done_once = true;
			}
			else if (event.ctrl.bRequestType == 0x01 && event.ctrl.bRequest == 0x0b) { // Set interface/alt_setting
				struct raw_gadget_config *config =
					&host_device_desc.configs[host_device_desc.current_config];

				int desired_interface = -1;
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					if (config->interfaces[i].altsettings[0].interface.bInterfaceNumber ==
							event.ctrl.wIndex) {
						desired_interface = i;
						break;
					}
				}
				if (desired_interface < 0) {
					printf("[Warning] Skip changing interface, wIndex(%d) is invalid\n", event.ctrl.wIndex);
					continue;
				}

				struct raw_gadget_interface *iface = &config->interfaces[desired_interface];

				int desired_altsetting = -1;
				for (int i = 0; i < iface->num_altsettings; i++) {
					if (iface->altsettings[i].interface.bAlternateSetting == event.ctrl.wValue) {
						desired_altsetting = i;
						break;
					}
				}
				if (desired_altsetting < 0) {
					printf("[Warning] Skip changing alt_setting, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				struct raw_gadget_altsetting *alt = &iface->altsettings[desired_altsetting];

				printf("Changing interface/altsetting\n");

				terminate_eps(fd, host_device_desc.current_config,
					desired_interface, iface->current_altsetting);
				set_interface_alt_setting(alt->interface.bInterfaceNumber,
					alt->interface.bAlternateSetting);
				process_eps(fd, host_device_desc.current_config,
					desired_interface, desired_altsetting);
				iface->current_altsetting = desired_altsetting;
			}
			else {
				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				memcpy(control_data, io.data, event.ctrl.wLength);

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "out");

				result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
				if (result == 0) {
					printf("ep0: transferred %d bytes (out)\n", rv);
				}
				else {
					usb_raw_ep0_stall(fd);
				}
			}
		}

		delete[] control_data;
	}

	struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		int interface_num = iface->altsettings[iface->current_altsetting]
			.interface.bInterfaceNumber;
		terminate_eps(fd, host_device_desc.current_config, i,
				iface->current_altsetting);
		release_interface(interface_num);
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}
