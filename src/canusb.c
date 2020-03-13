#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <asm/termbits.h> /* struct termios2 */
#include <time.h>
#include <syslog.h>
#include <net/if.h>
#include <linux/can.h>
#include <stdarg.h>
#include <stdbool.h>

#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include "interface.h"


// baudrate seems to be fixed for the device.
#define CAN_USB_BAUDRATE 2000000
#define RECV_STACK_SIZE 128

typedef enum {
  RECEIVING,
  COMPLETE,
  MISSED_HEADER
} FRAME_STATE;

int print_traffic = 0;
int print_frames = 0;
int can_usb_running = 0;
int listen_only = 0;
int exit_code = EXIT_SUCCESS;
char *tty_path = NULL;
int tty_fd = 0;
int can_soc_fd = 0;

// recv stack is used to store frames which have been read from the serial interface
// can_to_serial is reading this stack to prevent sending read frames back to to serial interface
int recvValidItemCount = 0;
typedef struct {
  struct can_frame frame;
  int valid;
} recvMsg;
recvMsg recvStack[RECV_STACK_SIZE];
pthread_mutex_t recvStackMutex = PTHREAD_MUTEX_INITIALIZER;

static void fake_syslog(int priority, const char *format, ...) {
  va_list ap;
  printf("[%d] ", priority);
  va_start(ap, format);
  vprintf(format, ap);
  va_end(ap);
  printf("\n");
}

typedef void (*syslog_t)(int priority, const char *format, ...);

static syslog_t sys_logger = syslog;

static void child_handler(const int signum) {
  switch (signum) {

	case SIGUSR1:
	  /* exit parent */
	  exit(EXIT_SUCCESS);
	  // no break needed due to exit
	case SIGALRM:
	case SIGCHLD:
	  sys_logger(LOG_NOTICE, "received signal %i on %s", signum, tty_path);
	  exit_code = EXIT_FAILURE;
	  can_usb_running = 0;
	  break;
	case SIGINT:
	case SIGTERM:
	  sys_logger(LOG_NOTICE, "received signal %i on %s", signum, tty_path);
	  exit_code = EXIT_SUCCESS;
	  can_usb_running = 0;
	  break;
	default:
	  break;
  }
}

CANUSB_SPEED canusb_int_to_speed(const int speed) {
  switch (speed) {
	case 1000000:
	  return CANUSB_SPEED_1000000;
	case 800000:
	  return CANUSB_SPEED_800000;
	case 500000:
	  return CANUSB_SPEED_500000;
	case 400000:
	  return CANUSB_SPEED_400000;
	case 250000:
	  return CANUSB_SPEED_250000;
	case 200000:
	  return CANUSB_SPEED_200000;
	case 125000:
	  return CANUSB_SPEED_125000;
	case 100000:
	  return CANUSB_SPEED_100000;
	case 50000:
	  return CANUSB_SPEED_50000;
	case 20000:
	  return CANUSB_SPEED_20000;
	case 10000:
	  return CANUSB_SPEED_10000;
	case 5000:
	  return CANUSB_SPEED_5000;
	default:
	  return CANUSB_SPEED_INVALID;
  }
}

int is_data_frame(const unsigned char *frame) {
  return (frame[1] >> 4) == 0xc;
}

int generate_checksum(const unsigned char *data, const int data_len) {
  int i, checksum;

  checksum = 0;
  for (i = 0; i < data_len; i++) {
	checksum += data[i];
  }

  return checksum & 0xff;
}

FRAME_STATE get_frame_state(unsigned char *frame, int frame_len) {
  if (frame_len > 0) {
	if (frame[0] != PACKET_START_FLAG) {
	  /* Need to sync on 0xaa at start of frames, so just skip. */
	  return MISSED_HEADER;
	}
  }

  if (frame_len < 2) {
	return RECEIVING;
  }

  if (frame[1] == 0x55) { /* Command frame... */
	if (frame_len >= 20) { /* ...always 20 bytes. */
	  return COMPLETE;
	} else {
	  return RECEIVING;
	}
  } else if (is_data_frame(frame)) { /* Data frame... */
	if (frame_len >= (frame[1] & 0xf) + 5) { /* ...payload and 5 bytes. */
	  return COMPLETE;
	} else {
	  return RECEIVING;
	}
  }

  /* Unhandled frame type. */
  return COMPLETE;
}

int frame_send(unsigned char *frame, int frame_len) {
  int result, i;

  if (print_traffic) {
	printf(">>> ");
	for (i = 0; i < frame_len; i++) {
	  printf("%02x ", frame[i]);
	}
	printf("\n");
  }

  result = (int) write(tty_fd, frame, (size_t) frame_len);
  if (result == -1) {
	sys_logger(LOG_ERR, "write() failed: %s", strerror(errno));
	return -2;
  }

  return frame_len;
}

int frame_recv(unsigned char *frame, int frame_len_max) {
  int frame_len, checksum;
  ssize_t result;
  unsigned char byte;

  if (print_traffic) {
	fprintf(stderr, "<<< ");
  }

  frame_len = 0;
  FRAME_STATE state = RECEIVING;
  while (state != COMPLETE && can_usb_running) {
	result = read(tty_fd, &byte, 1);
	if (result == -1) {
	  if (errno != EAGAIN && errno != EWOULDBLOCK) {
		sys_logger(LOG_ERR, "read() failed: %s", strerror(errno));
		return -1;
	  }

	} else if (result > 0) {
	  if (print_traffic) {
		fprintf(stderr, "%02x ", byte);
	  }

	  if (frame_len == frame_len_max) {
		sys_logger(LOG_ERR, "frame_recv() failed: Overflow");
		return -1;
	  }

	  frame[frame_len++] = byte;

	  state = get_frame_state(frame, frame_len);

	  // we can only handle complete serial frames, reset data.
	  if (MISSED_HEADER == state) {
		memset(frame, 0, (size_t) frame_len);
		frame_len = 0;
	  }
	}

	usleep(1000);
  }

  if (print_traffic) {
	fprintf(stderr, "\n");
  }

  /* Compare checksum for command frames only. */
  if ((frame_len == 20) && (frame[0] == PACKET_START_FLAG)
	  && (frame[1] == 0x55)) {
	checksum = generate_checksum(&frame[2], 17);
	if (checksum != frame[frame_len - 1]) {
	  sys_logger(LOG_ERR, "frame_recv() failed: Checksum incorrect");
	  return -1;
	}
  }

  return frame_len;
}

int command_settings(CANUSB_SPEED speed,
					 CANUSB_MODE mode,
					 CANUSB_FRAME_TYPE frame) {
  int cmd_frame_len;
  unsigned char cmd_frame[20];

  cmd_frame_len = 0;
  cmd_frame[cmd_frame_len++] = PACKET_START_FLAG;
  cmd_frame[cmd_frame_len++] = 0x55;
  cmd_frame[cmd_frame_len++] = 0x12;
  cmd_frame[cmd_frame_len++] = speed;
  cmd_frame[cmd_frame_len++] = frame;
  cmd_frame[cmd_frame_len++] = 0; /* Filter ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Filter ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Filter ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Filter ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Mask ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Mask ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Mask ID not handled. */
  cmd_frame[cmd_frame_len++] = 0; /* Mask ID not handled. */
  cmd_frame[cmd_frame_len++] = mode;
  cmd_frame[cmd_frame_len++] = 0x01;
  cmd_frame[cmd_frame_len++] = 0;
  cmd_frame[cmd_frame_len++] = 0;
  cmd_frame[cmd_frame_len++] = 0;
  cmd_frame[cmd_frame_len++] = 0;
  cmd_frame[cmd_frame_len++] =
	  (unsigned char) generate_checksum(&cmd_frame[2], 17);

  if (frame_send(cmd_frame, cmd_frame_len) < 0) {
	return -1;
  }

  return 0;
}

unsigned int get_frame_id(const unsigned char *frame,
						  const CANUSB_FRAME_TYPE type) {
  unsigned int id;
  if (CANUSB_FRAME_STANDARD == type) {
	id = ((frame[3] << 8) | frame[2]);
  } else {
	id = (frame[5] << 24) | (frame[4] << 16) | (frame[3] << 8) | (frame[2]);
  }

  return id;
}

/* 
    Get data from the serial adapter and 
    send it via can send to the virtual interface
*/
void serial_adapter_to_can(CANUSB_FRAME_TYPE type, char *adapter_name) {
  int i, c = 0, frame_len = 0;
  unsigned char frame[32];
  char buf[4096];
  char dbuf[16];

  int offset = 2;
  if (type == CANUSB_FRAME_STANDARD) {
	offset += 2;
  } else {
	offset += 4;
  }

  struct timespec ts;
  sys_logger(LOG_DEBUG, "Serial adapter to vcan handler running...");
  while (can_usb_running) {
	usleep(100);

	frame_len = frame_recv(frame, sizeof(frame));

	if (frame_len == -1) {
	  sys_logger(LOG_WARNING, "Frame receive error!");
	} else {
	  // only handle complete data frames
	  if ((frame_len >= 6) &&
		  is_data_frame(frame)) {

		// prepare data for print and for can send
		unsigned int frame_id = get_frame_id(frame, type);
		memset(dbuf, 0, sizeof dbuf);
		for (i = offset; i < frame_len - 1; i++) {
		  c += snprintf(dbuf + c, sizeof dbuf, "%02x", frame[i]);
		}

		// add data to recv stack
		if (!listen_only) {
		  pthread_mutex_lock(&recvStackMutex);

		  // find next free item
		  int found = 0;
		  for (i = 0; i < RECV_STACK_SIZE; i++) {
			if (recvStack[i].valid) {
			  continue;
			}
			recvStack[i].frame.can_id = frame_id;
			recvStack[i].frame.can_dlc = i;
			recvStack[i].valid = 1;
			memcpy(recvStack[i].frame.data, dbuf, i);
			recvValidItemCount++;
			found++;
			break;
		  }
		  assert(found != 0);
		  pthread_mutex_unlock(&recvStackMutex);
		}

		// print data to stdout
		if (print_frames) {
		  clock_gettime(CLOCK_MONOTONIC, &ts);
		  printf("%lu.%06lu ", ts.tv_sec, ts.tv_nsec / 1000);
		  printf("Frame ID: %02x, Data: %s\n", frame_id, dbuf);
		}

		// send data via vcan
		// run shell command for sending
		snprintf(buf,
				 sizeof buf,
				 "%s %s %03x#%s",
				 "cansend",
				 adapter_name,
				 frame_id,
				 dbuf);
		c = system(buf);
		if (c != 0) {
		  snprintf(buf,
				   sizeof buf,
				   "failed to send data via can, used command: %s %s %02x#%s",
				   "cansend",
				   adapter_name,
				   frame_id,
				   dbuf);
		  sys_logger(LOG_ERR, buf);
		}
	  }
	}
  }
}

int send_data_frame(const unsigned char *data,
					const int data_length,
					const CANUSB_FRAME_TYPE type,
					const int id) {
  unsigned char *frame;
  int frame_size = 3 + data_length;
  int data_index = 0, i;
  if (CANUSB_FRAME_STANDARD == type) {
	frame_size += 2;
  } else {
	frame_size += 4;
  }

  frame = (unsigned char *) malloc((size_t) frame_size);
  frame[0] = (unsigned char) PACKET_START_FLAG;
  frame[frame_size - 1] = PACKET_END_FLAG;

  if (CANUSB_FRAME_STANDARD == type) {
	frame[1] = (unsigned char) (0xc0 | data_length);
  } else {
	frame[1] = (unsigned char) (0xe0 | data_length);
  }

  if (CANUSB_FRAME_STANDARD == type) {
	frame[2] = (unsigned char) (id & 0xFF);
	frame[3] = (unsigned char) ((id >> 8) & 0xFF);
	data_index = 4;
  } else {
	frame[2] = (unsigned char) (id & 0xFF);
	frame[3] = (unsigned char) ((id >> 8) & 0xFF);
	frame[4] = (unsigned char) ((id >> 16) & 0xFF);
	frame[5] = (unsigned char) ((id >> 24) & 0xFF);
	data_index = 6;
  }

  for (i = data_index; i < data_index + data_length; i++) {
	frame[i] = data[i - data_index];
  }

  return frame_send(frame, frame_size);
}

int is_message_from_serial_port(struct can_frame frame_rd) {
  // check recv stack if this message is from serial bus.
  int i, j;
  if (recvValidItemCount == 0) {
	return 0;
  }

  pthread_mutex_lock(&recvStackMutex);
  for (i = 0; i < RECV_STACK_SIZE; i++) {
	if (!recvStack[i].valid) continue;
	bool equal = true;
	if (frame_rd.can_id == recvStack[i].frame.can_id &&
		frame_rd.can_dlc == recvStack[i].frame.can_dlc) {
	  // compare payload
	  for (j = 0; j < frame_rd.can_dlc; j++) {
		if (frame_rd.data[j] != recvStack[i].frame.data[j]) {
		  equal = false;
		  break;
		}
	  }
	} else {
	  equal = false;
	}

	// all things match. this is a frame we send to the adapter
	if (equal) {
	  recvStack[i].valid = false;
	  recvValidItemCount--;
	  pthread_mutex_unlock(&recvStackMutex);
	  return 1;
	}
  }

  pthread_mutex_unlock(&recvStackMutex);
  return 0;
}

/*
    Copy data from the bus to the serial adapter
*/
void *can_to_serial_adapter(void *arg) {
  // do not send any data when in listen only mode
  struct can_frame frame_rd;
  ssize_t recv_bytes = 0;

  struct timeval timeout = {1, 0};
  fd_set readSet;
//    FD_ZERO(&readSet);
//    FD_SET(can_soc_fd, &readSet);

  int i, j;

  CANUSB_FRAME_TYPE type = (CANUSB_FRAME_TYPE) arg;

  while (can_usb_running) {
	usleep(100);

	FD_ZERO(&readSet);
	FD_SET(can_soc_fd, &readSet);

	if (select((can_soc_fd + 1), &readSet, NULL, NULL, &timeout) < 0) {
	  continue;
	}

	if (!FD_ISSET(can_soc_fd, &readSet)) {
	  continue;
	}

	recv_bytes = read(can_soc_fd, &frame_rd, sizeof(struct can_frame));
	if (!recv_bytes) {
	  continue;
	}

	if (is_message_from_serial_port(frame_rd)) {
	  continue;
	}

	// print frames we received to std out.
	if (print_frames) {
	  printf("dlc = %d, data = ", frame_rd.can_dlc);
	  for (i = 0; i < frame_rd.can_dlc; i++) {
		printf("%02x", frame_rd.data[i]);
	  }
	  printf("\n");
	}

	// create a data frame and send it to the serial adapter
	i = send_data_frame(frame_rd.data, frame_rd.can_dlc, type, frame_rd.can_id);

	// fatal error like device disconnected
	if (i < -1) {
	  sys_logger(LOG_ERR,
				 "Application like will close due to fatal r/w error.");
	  can_usb_running = 0;
	}

  }

  return NULL;
}

int start_data_handling(CANUSB_FRAME_TYPE type, char *adapter_name) {
  if (listen_only) {
	serial_adapter_to_can(type, adapter_name);
  } else {
	pthread_t can_to_serial_thread;

	// create receive thread
	if (pthread_create(&can_to_serial_thread,
					   NULL,
					   &can_to_serial_adapter,
					   (void *) type)) {
	  sys_logger(LOG_ERR, "Error creating can to serial thread");
	  return -1;
	}

	// can to serial will run in a new thread,
	// serial to can will run in the current thread.
	serial_adapter_to_can(type, adapter_name);
	pthread_join(can_to_serial_thread, NULL);
  }

  return 0;
}

int init_can_socket(const char *port) {
  struct ifreq ifr;
  struct sockaddr_can addr;

  /* open socket */
  can_soc_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (can_soc_fd < 0) {
	return 0;
  }

  addr.can_family = AF_CAN;
  strcpy(ifr.ifr_name, port);

  if (ioctl(can_soc_fd, SIOCGIFINDEX, &ifr) < 0) {
	sys_logger(LOG_ERR, "ioctl() failed: %s", strerror(errno));
	return 0;
  }

  addr.can_ifindex = ifr.ifr_ifindex;

  fcntl(can_soc_fd, F_SETFL, O_NONBLOCK);

  if (bind(can_soc_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	sys_logger(LOG_ERR, "bind() failed: %s", strerror(errno));
	return 0;
  }

  return 1;
}

int init_serial_adapter(char *tty_device, int baudrate) {
  int result;
  struct termios2 tio;

  tty_fd = open(tty_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (tty_fd == -1) {
	sys_logger(LOG_ERR, "open(%s) failed: %s\n", tty_device, strerror(errno));
	return -1;
  }

  result = ioctl(tty_fd, TCGETS2, &tio);
  if (result == -1) {
	sys_logger(LOG_ERR, "ioctl() failed: %s\n", strerror(errno));
	close(tty_fd);
	return -1;
  }

  tio.c_cflag &= ~CBAUD;
  tio.c_cflag = BOTHER | CS8 | CSTOPB;
  tio.c_iflag = IGNPAR;
  tio.c_oflag = 0;
  tio.c_lflag = 0;
  tio.c_ispeed = (speed_t) baudrate;
  tio.c_ospeed = (speed_t) baudrate;

  result = ioctl(tty_fd, TCSETS2, &tio);
  if (result == -1) {
	sys_logger(LOG_ERR, "ioctl() failed: %s\n", strerror(errno));
	close(tty_fd);
	return -1;
  }

  return tty_fd;
}

void display_help(char *progname) {
  fprintf(stderr, "Usage: %s <options>\n", progname);
  fprintf(stderr, "Options:\n"
				  "  -h          Display this help and exit.\n"
				  "  -t          Print TTY/serial traffic debugging info on stderr.\n"
				  "  -d DEVICE   Use TTY DEVICE.\n"
				  "  -s SPEED    Set CAN SPEED in bps.\n"
				  "  -l          Set interface to listen only\n"
				  "  -p          Prints the data which will be sent via socket can to std out.\n"
				  "  -e          Set interface to extended frame mode. \n"
				  "  -F          Stay in foreground; no daemonize. \n"
				  "  -n NAME     Set name of can adapter.\n"
				  "\n"
				  "Example:\n"
				  "Start with 500kBaud, print trace and stay in foreground\n"
				  "usbcan -p -s 500000 -d /dev/ttyUSB0 -t -F\n\n"
				  "Open interface with 500kBaud and run as deamon:\n"
				  "usbcan -s 500000 -d /dev/ttyUSB0\n\n");
}

void teardown(char *can_adapter) {
  if (NULL != can_adapter) {
	char buf[256];
	int c;
	snprintf(buf, sizeof buf, "%s %s", "ip link delete dev", can_adapter);
	c = system(buf);
	if (c != 0) {
	  sys_logger(LOG_ERR, "failed to delete adapter");
	  exit(EXIT_FAILURE);
	} else {
	  sys_logger(LOG_DEBUG, "deleted can adapter");
	}
  }

  if (0 != can_soc_fd) {
	close(can_soc_fd);
  }
  if (0 != tty_fd) {
	close(can_soc_fd);
  }
}

int main(int argc, char *argv[]) {
  int c, tty_fd;
  CANUSB_SPEED speed = CANUSB_SPEED_INVALID;
  CANUSB_FRAME_TYPE type = CANUSB_FRAME_STANDARD;
  int baud_rate = CAN_USB_BAUDRATE;
  int run_as_daemon = 1;
  char *can_adapter = "slcan0";
  char buf[256];
  memset(recvStack, 0, RECV_STACK_SIZE);

  while ((c = getopt(argc, argv, "htd:s:b:n:eFpl")) != -1) {
	switch (c) {
	  case 'h':
		display_help(argv[0]);
		return EXIT_SUCCESS;

	  case 'l':
		listen_only = 1;
		break;

	  case 't':
		print_traffic = 1;
		break;

	  case 'd':
		tty_path = optarg;
		break;

	  case 's':
		speed = canusb_int_to_speed(atoi(optarg));
		break;

	  case 'p':
		print_frames = 1;
		break;

	  case 'F':
		run_as_daemon = 0;
		break;

	  case 'e':
		type = CANUSB_FRAME_EXTENDED;
		break;

	  case 'n':
		can_adapter = optarg;
		break;

	  case '?':
	  default:
		display_help(argv[0]);
		return EXIT_FAILURE;
	}
  }

  if (!run_as_daemon) {
	sys_logger = fake_syslog;
  }

  if (tty_path == NULL) {
	fprintf(stderr, "Please specify a TTY!\n");
	display_help(argv[0]);
	return EXIT_FAILURE;
  }

  if (speed == CANUSB_SPEED_INVALID) {
	fprintf(stderr, "Please specify a valid speed!\n");
	display_help(argv[0]);
	return EXIT_FAILURE;
  }

  tty_fd = init_serial_adapter(tty_path, baud_rate);
  if (tty_fd == -1) {
	return EXIT_FAILURE;
  }

  // load the kernel module
  // todo request root rights
  c = system("modprobe vcan");
  if (c != 0) {
	sys_logger(LOG_ERR, "failed to load vcan module");
	teardown(NULL);
	return EXIT_FAILURE;
  }

  snprintf(buf,
		   sizeof buf,
		   "%s %s %s",
		   "ip link add dev",
		   can_adapter,
		   "type vcan");
  c = system(buf);
  if (c != 0) {
	sys_logger(LOG_ERR, "failed to create can adapter");
	teardown(NULL);
	return EXIT_FAILURE;
  }

  snprintf(buf, sizeof buf, "%s %s", "ip link set up", can_adapter);
  c = system(buf);
  if (c != 0) {
	sys_logger(LOG_ERR, "failed to bring adapter up");
	teardown(can_adapter);
	return EXIT_FAILURE;
  }

  if (!init_can_socket(can_adapter)) {
	sys_logger(LOG_ERR, "failed to setup socket can");
	teardown(NULL);
	return EXIT_FAILURE;
  }



  /* Daemonize */
  if (run_as_daemon) {
	if (daemon(0, 0)) {
	  sys_logger(LOG_ERR, "failed to daemonize");
	  teardown(can_adapter);
	  exit(EXIT_FAILURE);
	}
  } else {
	sys_logger(LOG_DEBUG, "Running in foreground process");
  }

  /* Trap signals that we expect to receive */
  signal(SIGINT, child_handler);
  signal(SIGTERM, child_handler);

  can_usb_running = 1;

  // configure interface
  CANUSB_MODE mode = CANUSB_MODE_NORMAL;
  if (listen_only) {
	mode = CANUSB_MODE_SILENT;
	sys_logger(LOG_INFO, "Staring in listen mode");
  }

  // configure the interface
  command_settings(speed, mode, type);

  // start handling
  if (-1 == start_data_handling(type, can_adapter)) {
	sys_logger(LOG_ERR, "failed to start communication listener");
	exit_code = EXIT_FAILURE;
  }

  // cleanup
  teardown(can_adapter);

  return exit_code;
}

