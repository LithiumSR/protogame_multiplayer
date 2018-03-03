#include <GL/glut.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>  // htons() and inet_addr()
#include <netinet/in.h> // struct sockaddr_in
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include "common.h"
#include "image.h"
#include "surface.h"
#include "world.h"
#include "vehicle.h"
#include "world_viewer.h"
#include "client_op.h"
#include "so_game_protocol.h"
#include <fcntl.h>

int window;
World world;
Vehicle* vehicle; // The vehicle
int id;
uint16_t  port_number_no;
int connectivity=1;
int checkUpdate=1;
int socket_desc; //socket tcp

typedef struct localWorld{
    int  ids[WORLDSIZE];
    int users_online;
    Vehicle** vehicles;
}localWorld;

typedef struct listenArgs{
    localWorld* lw;
    struct sockaddr_in server_addr;
    int socket_udp;
    int socket_tcp;
}udpArgs;

void handle_signal(int signal){
    // Find out which signal we're handling
    switch (signal) {
        case SIGHUP:
            break;
        case SIGINT:
            connectivity=0;
            checkUpdate=0;
            send_goodbye(socket_desc, id);
            break;
        default:
            fprintf(stderr, "Caught wrong signal: %d\n", signal);
            return;
    }
}

int add_user_id(int ids[] , int size , int id, int* position){
    for(int i=0;i<size;i++){
        if(ids[i]==id) return i;
    }
    for (int i=0 ; i < size ; i++){
        if(ids[i]!=0){
            ids[i]=id;
            *position=i;
            break;
        }
    }
    return -1;
}

int sendUpdates(int socket_udp, localWorld* lw, struct sockaddr_in server_addr,int serverlen){
    char buf_send[BUFFSIZE];
    debug_print("[UDP_Sender] Sending VehicleUpdatePacket");
    PacketHeader ph;
    ph.type=VehicleUpdate;
    VehicleUpdatePacket* vup=(VehicleUpdatePacket*)malloc(sizeof(VehicleUpdatePacket));
    vup->header=ph;
    getForces(vehicle,&(vup->translational_force),&(vup->rotational_force));
    getXYTheta(vehicle,&(vup->x),&(vup->y),&(vup->theta));
    vup->id=id;
    vup->time=time(NULL);
    int size=Packet_serialize(buf_send, &vup->header);
    int bytes_sent = sendto(socket_udp, buf_send, size, 0, (struct sockaddr *) &server_addr,(socklen_t) serverlen);
    if(bytes_sent<0) return -1;
    else {
        debug_print("[UDP_Sender] VehicleUpdatePacket sent");
        return 0;
    }
}

void* sender_routine(void* args){
    udpArgs udp_args =*(udpArgs*)args;
    struct sockaddr_in server_addr=udp_args.server_addr;
    int socket_udp =udp_args.socket_udp;
    int serverlen=sizeof(server_addr);
    while(connectivity && checkUpdate){
        int ret=sendUpdates(socket_udp,udp_args.lw,server_addr,serverlen);
        if(ret==-1) debug_print("[UDP_Sender] Cannot send VehicleUpdatePacket");
        sleep(1000);
    }
    pthread_exit(NULL);
}

void* receiver_routine(void* args){
    udpArgs udp_args =*(udpArgs*)args;
    struct sockaddr_in server_addr=udp_args.server_addr;
    int socket_udp =udp_args.socket_udp;
    int serverlen=sizeof(server_addr);
    localWorld* lw=udp_args.lw;
    int socket_tcp=udp_args.socket_tcp;
    while(connectivity && checkUpdate){
        char buf_rcv[BUFFSIZE];
        int bytes_read=recvfrom(socket_udp, buf_rcv, BUFFSIZE, 0, (struct sockaddr*) &server_addr, (socklen_t*) &serverlen);
        if(bytes_read==-1){
            debug_print("[UDP_Receiver] Errore while receiving WorldUpdatePacket");
            connectivity=0;
            break;
        }
        if (bytes_read==0) continue;
        PacketHeader* ph=(PacketHeader*)buf_rcv;
        if(ph->type!=WorldUpdate) continue;
        WorldUpdatePacket* wup = (WorldUpdatePacket*)Packet_deserialize(buf_rcv, bytes_read);
        if ( wup->num_vehicles <= lw->users_online ) continue;
        for(int i=0; i < wup -> num_vehicles ; i++){
            if(wup->updates[i].id==id) continue;
            int new_position=-1;
            int id_struct=add_user_id(lw->ids,WORLDSIZE,wup->updates[i].id,&new_position);
            if(id_struct==-1){
                Image * img = get_vehicle_texture(socket_tcp,wup->updates[i].id);
                Vehicle* new_vehicle=(Vehicle*) malloc(sizeof(Vehicle));
                Vehicle_init(new_vehicle,&world,wup->updates[i].id,img);
                lw->vehicles[new_position]=new_vehicle;
                setXYTheta(lw->vehicles[new_position],wup->updates[i].x,wup->updates[i].y,wup->updates[i].theta);
                setForces(lw->vehicles[new_position],wup->updates[i].translational_force,wup->updates[i].rotational_force);
            }
            else {
                setXYTheta(lw->vehicles[id_struct],wup->updates[i].x,wup->updates[i].y,wup->updates[i].theta);
                setForces(lw->vehicles[id_struct],wup->updates[i].translational_force,wup->updates[i].rotational_force);
            }

        }
        sleep(1000);
    }
    pthread_exit(NULL);
}

int createUDPSocket(struct sockaddr_in* si_me, int port){
    int socket_udp;
    socket_udp=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ERROR_HELPER(socket_udp,"Something went wrong during the creation of the udp socket");
    memset((char *) &si_me, 0, sizeof(si_me));
    in_addr_t ip_addr = inet_addr(SERVER_ADDRESS);
    si_me->sin_family = AF_INET;
    si_me->sin_port = htons(port);
    si_me->sin_addr.s_addr = ip_addr;
    int ret=bind(socket_udp, (struct sockaddr*)&si_me, sizeof(si_me) );
    ERROR_HELPER(ret,"Error during bind");
    return socket_udp;
}

int main(int argc, char **argv) {
    if (argc<2) {
        printf("usage: %s <player texture> \n", argv[1]);
        exit(-1);
        }
    debug_print("[Main] loading texture image from %s ... ", argv[1]);
    Image* my_texture = Image_load(argv[1]);
    if (my_texture) {
        printf("Done! \n");
        }
    else {
        printf("Fail! \n");
        }


    /**
    long tmp = strtol(argv[4], NULL, 0);
    if (tmp < 1024 || tmp > 49151) {
        fprintf(stderr, "Use a port number between 1024 and 49151.\n");
        exit(EXIT_FAILURE);
        }
    **/
    long tmp=SERVER_PORT;
    debug_print("[Main] Starting... \n");
    port_number_no = htons((uint16_t)tmp); // we use network byte order
	socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    in_addr_t ip_addr = inet_addr(SERVER_ADDRESS);
    ERROR_HELPER(socket_desc, "Cannot create socket \n");
	struct sockaddr_in server_addr = {0}; // some fields are required to be filled with 0
    server_addr.sin_addr.s_addr = ip_addr;
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = port_number_no;
	int ret= connect(socket_desc, (struct sockaddr*) &server_addr, sizeof(struct sockaddr_in));
    ERROR_HELPER(ret, "Cannot connect to remote server \n");
    debug_print("[Main] TCP connection established... \n");

    //seting signal handlers
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    // Restart the system call, if at all possible
    sa.sa_flags = SA_RESTART;
    // Block every signal during the handler
    sigfillset(&sa.sa_mask);
    ret=sigaction(SIGHUP, &sa, NULL);
    ERROR_HELPER(ret,"Error: cannot handle SIGHUP");
    ret=sigaction(SIGINT, &sa, NULL);
    ERROR_HELPER(ret,"Error: cannot handle SIGINT");

    //setting non-blocking socket
    fcntl(socket_desc, F_SETFL, fcntl(socket_desc, F_GETFL) | O_NONBLOCK);

    debug_print("[Main] Starting ID,map_elevation,map_texture requests \n");
    int id=get_client_ID(socket_desc);
    debug_print("[Main] ID received \n");
    Image* map_elevation=get_image_elevation(socket_desc,id);
    debug_print("[Main] Map elevation received \n");
    Image* map_texture = get_image_texture(socket_desc,id);
    debug_print("[Main] Map texture received \n");
    debug_print("[Main] Sending vehicle texture");
    send_vehicle_texture(socket_desc,id,my_texture);
    debug_print("[Main] Client Vehicle texture sent \n");


    localWorld* myLocalWorld = (localWorld*)malloc(sizeof(localWorld));
    myLocalWorld->vehicles=(Vehicle**)malloc(sizeof(Vehicle*)*WORLDSIZE);

    // construct the world
    World_init(&world, map_elevation, map_texture,0.5, 0.5, 0.5);
    vehicle=(Vehicle*) malloc(sizeof(Vehicle));
    Vehicle_init(vehicle, &world, id, my_texture);
    World_addVehicle(&world, vehicle);

    // spawn a thread that will listen the update messages from
    // the server, and sends back the controls
    // the update for yourself are written in the desired_*_force
    // fields of the vehicle variable
    // when the server notifies a new player has joined the game
    // request the texture and add the player to the pool

    struct sockaddr_in udp_addr;
    int socket_udp=createUDPSocket(&udp_addr,tmp);

    pthread_t threadUDP_receiver,threadUDP_sender;
    udpArgs udp_args;
    udp_args.server_addr=udp_addr;
    udp_args.lw=myLocalWorld;
    udp_args.socket_udp=socket_udp;
    udp_args.socket_tcp=socket_desc;
    ret=pthread_create(&threadUDP_sender,NULL,sender_routine,&udp_args);
    PTHREAD_ERROR_HELPER(ret,"pthread_create on threadUDP_sender failed \n");
    ret=pthread_create(&threadUDP_receiver,NULL,receiver_routine,&udp_args);
    PTHREAD_ERROR_HELPER(ret,"pthread_create on threadUDP_receiver failed \n");
    WorldViewer_runGlobal(&world, vehicle, &argc, argv);
    debug_print("[Main] Closing the client... \n");
    ret=pthread_join(threadUDP_sender,NULL);
    PTHREAD_ERROR_HELPER(ret,"pthread_join on threadUDP_sender failed \n");
    ret=pthread_join(threadUDP_receiver,NULL);
    PTHREAD_ERROR_HELPER(ret,"pthread_join on threadUDP_receiver failed \n");

    // check out the images not needed anymore

    Image_free(my_texture);
    Image_free(map_elevation);
    Image_free(map_texture);

    free(myLocalWorld->vehicles);
    free(myLocalWorld);

    // world cleanup
    World_destroy(&world);
    return 0;
}
