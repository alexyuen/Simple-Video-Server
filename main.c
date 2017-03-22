#include "cloud_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

#include <cv.h>
#include <highgui.h>

#include <sys/time.h>
#include <signal.h>
#include <time.h>

#define BACKLOG 10     // how many pending connections queue will hold

int send_all(int s, void *buf, int len) {
	int total = 0;        // how many bytes we've sent
	int bytesleft = len; // how many we have left to send
	int n;

	while(total < len) {
		n = send(s, buf+total, bytesleft, MSG_NOSIGNAL);
		if (n == -1) { break; }
		total += n;
		bytesleft -= n;
	}

	len = total; // return number actually sent here

	return n==-1?-1:0; // return -1 on failure, 0 on success
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*) sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*) sa)->sin6_addr);
}

void dispatch(int sockfd, char * msg) {
	printf("%s\n", msg);
	int len, bytes_sent;
	len = strlen(msg);
	bytes_sent = send(sockfd, msg, len, MSG_NOSIGNAL);
	if (bytes_sent != len) {
		printf("Dispatch: The entire reply was not sent!\n");
		if (bytes_sent == -1) {
			printf("Dispatch: The connection was closed by the client.\n");
		}
	}
}

void reply(int sockfd, int cseq, int session) {
	char * s = malloc(snprintf(NULL, 0, "RTSP/1.0 200 OK\nCSeq: %d\nSession: %d\n\n",
			cseq, session) + 1);
	sprintf(s, "RTSP/1.0 200 OK\nCSeq: %d\nSession: %d\n\n", cseq, session);
	dispatch(sockfd, s);
	free(s);
}

void sendError(int sockfd) {
	dispatch(sockfd,
			"RTSP/1.0 455 Method Not Allowed in This State\nCSeq: -1\nSession: -1\n\n");
}

// This struct is created to save information that will be needed by the timer,
// such as socket file descriptors, frame numbers and video captures.
struct send_frame_data {
	int socket_fd;
	// other fields
	CvCapture *video;
	int scale;
	int frame_number;

	int is_cloud;
	const struct cloud_server * cloud;
	int cloud_fd;
	char * video_name;

	timer_t play_timer;
	struct itimerspec play_interval;
};

void openFile(const char* filename, struct send_frame_data* data) {
	// Open the video file.
	data->video = cvCaptureFromFile(filename);
	if (!data->video) {
		printf("The file doesn't exist or can't be captured as a video file.\n");
	} else {
		printf("Video loaded successfully\n");
	}
}

void closeFile(struct send_frame_data* data) {
	// Close the video file
	if (!data->is_cloud)
		cvReleaseCapture(&(data->video));
}


void stopTimer(struct send_frame_data* data) {
	// The following snippet is used to stop a currently running timer. The current
	// task is not interrupted, only future tasks are stopped.
	data->play_interval.it_interval.tv_sec = 0;
	data->play_interval.it_interval.tv_nsec = 0;
	data->play_interval.it_value.tv_sec = 0;
	data->play_interval.it_value.tv_nsec = 0;
	timer_settime(data->play_timer, 0, &data->play_interval, NULL );
}

int connect_to(char * server, int port) {
	int sockfd = 0;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	char port_str[15];
	sprintf(port_str, "%d", port);
	//printf("%s\n", port_str);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(server, port_str, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return sockfd;
	}

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("client: connect");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		return sockfd;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			s, sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure
	return sockfd;
}

// This function will be called when the timer ticks
void send_frame(union sigval sv_data) {
	struct send_frame_data *data = (struct send_frame_data *) sv_data.sival_ptr;
	// You may retrieve information from the caller using data->field_name
	if (data->is_cloud) {
		const struct cloud_server * next_server = get_cloud_server(data->video_name, data->frame_number);
		if (next_server == NULL) {
			printf("Server not found. Skipping frame...\n\n");
			data->frame_number+= data->scale;
			return;
		}
		if (data->frame_number == 0 || data->cloud_fd == 0 || (strcmp(next_server->server, data->cloud->server) != 0 && next_server->port != data->cloud->port)) {
			if (data->cloud_fd != 0) {
				close(data->cloud_fd);
			}
			data->cloud = next_server;
			printf("Connecting to new server %s:%d\n", next_server->server, next_server->port);
			data->cloud_fd = connect_to(next_server->server, next_server->port);
			if (data->cloud_fd == 0) {
				printf("Server not found. Skipping frame...\n\n");
				data->frame_number+= data->scale;
				return;
			}
		}
		// send request for next frame
		char * s = malloc(snprintf(NULL, 0, "%s:%d\n", data->video_name, data->frame_number) + 1);
		sprintf(s, "%s:%d\n", data->video_name, data->frame_number);
		dispatch(data->cloud_fd, s);
		data->frame_number+= data->scale;

		// read frame reply
		char buf[5];
		if (recv(data->cloud_fd, buf, 5, MSG_WAITALL) == -1) {
			printf("Error receiving frame. Skipping frame...\n\n");
			data->frame_number+= data->scale;
			return;
		}
		printf("client: received '%s'\n",buf);
		int frame_size = atoi(buf);
		printf("Frame size: %d\n", frame_size);
		if (frame_size == 0) {
			// end of video
			stopTimer(data);
			return;
		}

		char frame[frame_size];
		int numbytes;
		if ((numbytes = recv(data->cloud_fd, frame, frame_size, MSG_WAITALL)) == -1) {
			printf("Error receiving frame. Skipping frame...\n\n");
			data->frame_number+= data->scale;
			return;
		}
		printf("Frame bytes received: %d\n", numbytes);

		int length = 12 + frame_size;

		// send frame to client
		unsigned char pkt[16];
		pkt[0] = 0x24;
		pkt[1] = 0;
		pkt[2] = length >> 8;
		pkt[3] = length & 0xFF;
		// header stuff
		pkt[4] = 128; //version, padding, extension, CSRC count ie 10000000
		pkt[5] = 26;
		pkt[6] = data->frame_number >> 8;
		pkt[7] = data->frame_number & 0xFF;
		int stamp = data->frame_number * 40 * data->scale;
		pkt[8] = stamp >> 24;
		pkt[9] = stamp >> 16;
		pkt[10] = stamp >> 8;
		pkt[11] = stamp & 0xFF;

		send_all(data->socket_fd, pkt, 16);
		send_all(data->socket_fd, frame, frame_size);
		return;
	} else {
		IplImage *image;
		CvMat *thumb;
		CvMat *encoded;

		printf("Parsing next frame...\n");
		// Obtain the next frame from the video file
		int i = 0;
		while (i < data->scale) {
			image = cvQueryFrame(data->video);
			i++;
		}
		data->frame_number++;
		if (!image) {
			printf("Next frame doesn't exist or can't be obtained.\n");
			stopTimer(data);
			return;
		}

		// Convert the frame to a smaller size (WIDTH x HEIGHT)
		int HEIGHT = 480;
		int WIDTH = 640;
		thumb = cvCreateMat(HEIGHT, WIDTH, CV_8UC3);
		cvResize(image, thumb, CV_INTER_AREA);

		// Encode the frame in JPEG format with JPEG quality 30%.
		const static int encodeParams[] = { CV_IMWRITE_JPEG_QUALITY, 30 };
		encoded = cvEncodeImage(".jpeg", thumb, encodeParams);
		// After the call above, the encoded data is in encoded->data.ptr
		// and has a length of encoded->cols bytes.

		printf("Frame parsed, length: %d\n", encoded->cols);
		// send the frame in a RTP packet
		int length = 12 + encoded->cols;

		unsigned char pkt[16];
		pkt[0] = 0x24;
		pkt[1] = 0;
		pkt[2] = length >> 8;
		pkt[3] = length & 0xFF;
		//header stuff
		pkt[4] = 128; //version, padding, extension, CSRC count ie 10000000
		pkt[5] = 26;
		pkt[6] = data->frame_number >> 8;
		pkt[7] = data->frame_number & 0xFF;
		int stamp = data->frame_number * 40 * data->scale;
		pkt[8] = stamp >> 24;
		pkt[9] = stamp >> 16;
		pkt[10] = stamp >> 8;
		pkt[11] = stamp & 0xFF;

		send_all(data->socket_fd, pkt, 16);
		send_all(data->socket_fd, encoded->data.ptr, encoded->cols);
	}
}

void startTimer(struct send_frame_data * data) {
	// The following snippet is used to create and start a new timer that runs every 40 ms.
	struct sigevent play_event;

	memset(&play_event, 0, sizeof(play_event));
	play_event.sigev_notify = SIGEV_THREAD;
	play_event.sigev_value.sival_ptr = data;
	play_event.sigev_notify_function = send_frame; // call the send_frame function

	data->play_interval.it_interval.tv_sec = 0;
	data->play_interval.it_interval.tv_nsec = 40 * 1000000; // 40 ms in ns
	data->play_interval.it_value.tv_sec = 0;
	data->play_interval.it_value.tv_nsec = 1; // can't be zero

	timer_create(CLOCK_REALTIME, &play_event, &data->play_timer);
	timer_settime(data->play_timer, 0, &data->play_interval, NULL );
}

// handle a client
void *serve_client(void *ptr) {
	// converts ptr back to integer
	int client_fd = (int) (intptr_t) ptr;
	char * state = "INIT";

	int MAXBUFLEN = 100;
	char buf[MAXBUFLEN];

	srand(time(NULL ));
	int session = rand();

	struct send_frame_data data;
	data.socket_fd = client_fd;

	while (1) {
		char * sp;
		int numbytes = recv(client_fd, buf, MAXBUFLEN, 0);
		if (numbytes <= 0) {
			printf("The connection was closed by the client.\n");
			stopTimer(&data);
			timer_delete(data.play_timer);
			closeFile(&data);
			break;
		}

		//printf("listener: packet is %d bytes long\n", numbytes);
		buf[numbytes] = '\0';
		printf("Packet received:\n%s\n", buf);

		char * split = strtok_r(buf, " \n", &sp); // gets the first token
		if (strcmp(state, "INIT") == 0) {
			if (strcmp(split, "SETUP") == 0) {
				//C: SETUP movie.Mjpeg RTSP/1.0
				//C: CSeq: 1
				//C: Transport: RTP/TCP; interleaved=0
				//C:

				char * path = strtok_r(NULL, " \n", &sp); // gets the second token

				if (strstr(path, "cloud://") != NULL) {
					char cloud_path[64];
					sscanf(path, "cloud://%63[^\n]", cloud_path);
					data.is_cloud = 1;
					data.video_name = cloud_path;
					get_cloud_server(cloud_path, 0);
				} else {
					// open the file
					data.is_cloud = 0;
					openFile(path, &data);
				}

				char * format = strtok_r(NULL, " \n", &sp);
				printf("Format: %s\n", format);

				strtok_r(NULL, " \n", &sp);

				int cseq = atoi(strtok_r(NULL, " \n", &sp));
				printf("CSeq: %d\n", cseq);

				//printf("port: %d", data.port);

				reply(client_fd, cseq, session);
				state = "READY";
			} else {
				sendError(client_fd);
			}
		} else if (strcmp(state, "READY") == 0) {
			if (strcmp(split, "PLAY") == 0) {
				//PLAY sample.avi RTSP/1.0
				//CSeq: 2
				//Scale: 1
				//Session: 375664820
				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);

				int cseq = atoi(strtok_r(NULL, " \n", &sp));

				strtok_r(NULL, " \n", &sp);
				int scale = atoi(strtok_r(NULL, " \n", &sp));
				data.scale = scale;
				//printf("%d\n", scale);

				reply(client_fd, cseq, session);

				startTimer(&data);
				state = "PLAYING";
			} else if (strcmp(split, "TEARDOWN") == 0) {
				//TEARDOWN sample.avi RTSP/1.0
				//CSeq: 2
				//Session: 401819897
				stopTimer(&data);
				timer_delete(data.play_timer);
				data.frame_number = 0;
				closeFile(&data);

				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);

				int cseq = atoi(strtok_r(NULL, " \n", &sp));

				reply(client_fd, cseq, session);

				state = "INIT";
			} else {
				sendError(client_fd);
			}
		} else if (strcmp(state, "PLAYING") == 0) {
			if (strcmp(split, "PAUSE") == 0) {
				//C: PAUSE movie.Mjpeg RTSP/1.0
				//C: CSeq: 3
				//C: Session: 123456
				//C:
				stopTimer(&data);

				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);
				int cseq = atoi(strtok_r(NULL, " \n", &sp));
				reply(client_fd, cseq, session);

				state = "READY";
			} else if (strcmp(split, "TEARDOWN") == 0) {
				stopTimer(&data);
				timer_delete(data.play_timer);
				data.frame_number = 0;
				closeFile(&data);

				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);
				strtok_r(NULL, " \n", &sp);

				int cseq = atoi(strtok_r(NULL, " \n", &sp));

				reply(client_fd, cseq, session);

				state = "INIT";
			} else {
				sendError(client_fd);
			}
		} else {
			// invalid state
			sendError(client_fd);
		}
	}
	close(client_fd);
	return NULL ;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Please specify a port to listen on.");
		return 0;
	}
	int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, argv[1], &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL ; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))
				== -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))
				== -1) {
			perror("setsockopt");
			exit(1);
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			continue;
		}

		break;
	}

	if (p == NULL ) {
		fprintf(stderr, "server: failed to bind\n");
		return 2;
	}

	freeaddrinfo(servinfo); // all done with this structure

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	printf("server: waiting for connections...\n");

	while (1) {  // main accept() loop
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *) &their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
		printf("server: got connection from %s\n", s);

		// Creates the thread and calls the function serve_client.
		pthread_t thread;
		pthread_create(&thread, NULL, serve_client, (void *) (intptr_t) new_fd);
		// Detaches the thread. This means that, once the thread finishes, it is destroyed.
		pthread_detach(thread);
	}

	return 0;
}
