#include "ef_send_tcp.hpp"

int main(int argc, char *argv[])
{
    ef_init_tcp_client();
    ef_connect();
    ef_send("Hello HFTT Class\n", 17);
    ef_send("My name is Kevin\n", 17);
    ef_send("What is your name?\n", 19);
    ef_disconnect();
    return 0;
}