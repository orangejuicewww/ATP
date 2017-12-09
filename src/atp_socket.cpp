#include "atp_impl.h"
#include "udp_util.h"

ATPSocket::ATPSocket(ATPContext * _context) : context(_context){
    assert(context != nullptr);
    sock_id = context->new_sock_id();
    conn_state = CS_UNINITIALIZED;
    memset(hash_str, 0, sizeof hash_str);

    memset(callbacks, 0, sizeof callbacks);
    init_callbacks(this);
}

void ATPSocket::register_to_look_up(){
    // if not registered, can't find `ATPSocket *` by (addr:port)
    std::map<uint16_t, ATPSocket*>::iterator iter = context->listen_sockets.find(get_src_addr().host_port());
    if(iter != context->listen_sockets.end()){
        context->listen_sockets.erase(iter);
    }
    (context->look_up)[ATPSocket::make_hash_code(sock_id, dest_addr)] = this;
}


int ATPSocket::init(int family, int type, int protocol){
    conn_state = CS_IDLE;
    sockfd = socket(family, type, protocol);
    get_src_addr().family() = family;
    dest_addr.family() = family;
    #if defined (ATP_LOG_AT_DEBUG) && defined(ATP_LOG_UDP)
        log_debug(this, "UDP Socket init, sockfd %d.", sockfd);
    #endif
    return sockfd;
}

ATP_PROC_RESULT ATPSocket::connect(const ATPAddrHandle & to_addr){
    assert(context != nullptr);
    dest_addr = to_addr;

    assert(conn_state == CS_IDLE);
    conn_state = CS_SYN_SENT;
    register_to_look_up();

    seq_nr = rand() & 0xffff;
    ack_nr = 0;

    OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_SYN));
    add_data(out_pkt, &sock_id, sizeof(sock_id));

    // before sending packet, users can do something, like call `connect` to their UDP socket.
    atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_CONNECT, out_pkt, dest_addr);
    ATP_PROC_RESULT result = invoke_callback(ATP_CALL_CONNECT, &arg);

    #if defined (ATP_LOG_AT_DEBUG) && defined(ATP_LOG_UDP)
        log_debug(this, "UDP socket connect to %s.", dest_addr.to_string());
    #endif
    if (result == ATP_PROC_ERROR){

    } else{
        result = send_packet(out_pkt);
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "Sent SYN to peer, seq:%u.", out_pkt -> get_head() -> seq_nr);
        #endif
    }
    return result;
}


ATP_PROC_RESULT ATPSocket::listen(uint16_t host_port){
    conn_state = CS_LISTEN;
    // register to listen
    get_src_addr().set_port(host_port);
    if (context->listen_sockets.find(host_port) != context->listen_sockets.end())
    {
        context->listen_sockets[host_port] = this;
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "Listening port %u.", host_port);
        #endif
        return ATP_PROC_OK;
    }else{
        return ATP_PROC_ERROR;
    }
}

ATP_PROC_RESULT ATPSocket::bind(const ATPAddrHandle & to_addr){
    // there's no OutgoingPacket to be sent, so pass `nullptr`
    atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_BIND, nullptr, to_addr);
    ATP_PROC_RESULT result = invoke_callback(ATP_CALL_BIND, &arg);
    return result;
}

ATP_PROC_RESULT ATPSocket::accept(const ATPAddrHandle & to_addr, OutgoingPacket * recv_pkt){
    assert(context != nullptr);
    dest_addr = to_addr;

    assert(conn_state == CS_IDLE || conn_state == CS_LISTEN);
    conn_state = CS_SYN_RECV;
    register_to_look_up();
    peer_sock_id = *reinterpret_cast<uint16_t*>(recv_pkt->data + sizeof(ATPPacket));
    seq_nr = rand() & 0xffff;
    // must set ack_nr, because now ack_nr is still 0
    ack_nr = recv_pkt->get_head()->seq_nr;

    OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_SYN, PACKETFLAG_ACK));
    add_data(out_pkt, &sock_id, sizeof(sock_id));
    ATP_PROC_RESULT result = send_packet(out_pkt);

    #if defined (ATP_LOG_AT_DEBUG)
        log_debug(this, "Accept SYN request from %s by sending SYN+ACK."
            , ATPSocket::make_hash_code(peer_sock_id, dest_addr));
    #endif

    return result;
}

ATP_PROC_RESULT ATPSocket::receive(OutgoingPacket * recv_pkt){
    if (recv_pkt->get_head()->get_fin())
    {
        // cond2: ignore fin
        // just ignore
        return ATP_PROC_OK;
    }else if(recv_pkt->get_head()->get_syn()){
        // the 2 bytes payload in syn packet are not user data, they carried sock_id
        return ATP_PROC_OK;
    }else if(recv_pkt->payload == 0){
        // there is payload
        // just ignore
        return ATP_PROC_OK;
    }else{
        atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_ON_RECV, recv_pkt, dest_addr);
        arg.data = recv_pkt->data + sizeof(ATPPacket);
        arg.length = recv_pkt->payload;
        return invoke_callback(ATP_CALL_ON_RECV, &arg);
    }
}

ATP_PROC_RESULT ATPSocket::send_packet(OutgoingPacket * out_pkt){
    uint64_t current_ms = get_current_ms();
    rto_timeout = current_ms + rto;

    // when the package is constructed, update `seq_nr` for the next package
    seq_nr++;
    cur_window_packets++;

    out_pkt->timestamp = get_current_ms();
    out_pkt->transmissions++;
    #if defined (ATP_LOG_AT_DEBUG)
        log_debug(this, "ATPPacket sent. seq:%u size:%u payload:%u", out_pkt->get_head()->seq_nr, out_pkt->length, out_pkt->payload);
    #endif

    #if defined (ATP_LOG_AT_NOTE)
        print_out(this, out_pkt, "snd");
    #endif

    outbuf.push(out_pkt);

    // udp send
    atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_SENDTO, out_pkt, dest_addr);
    ATP_PROC_RESULT result = invoke_callback(ATP_CALL_SENDTO, &arg);

    #if defined (ATP_LOG_AT_DEBUG) && defined(ATP_LOG_UDP)
        log_debug(this, "UDP Send %u bytes.", result);
    #endif

    return result;
}

ATP_PROC_RESULT ATPSocket::close(){
    int result = ATP_PROC_OK;
    switch(conn_state){
        case CS_UNINITIALIZED:
        case CS_IDLE:
        case CS_SYN_SENT:
        case CS_SYN_RECV:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        case CS_CONNECTED:
        case CS_CONNECTED_FULL:
        {
            // A
            conn_state = CS_FIN_WAIT_1;
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Send FIN Packet.");
            #endif
            OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_FIN));
            send_packet(out_pkt);
            result = ATP_PROC_OK;
            break;
        }
        case CS_FIN_WAIT_1:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Already sent FIN, waiting for ACK.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        case CS_CLOSE_WAIT:
        {
            // B
            conn_state = CS_LAST_ACK;
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Send FIN Packet to a finished peer.");
            #endif
            OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_FIN));
            send_packet(out_pkt);
            result = ATP_PROC_OK;
            break;
        }
        case CS_FIN_WAIT_2:
        case CS_LAST_ACK:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        case CS_TIME_WAIT:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Send a repeated last ACK.");
            #endif
            result = ATP_PROC_OK;
            break;
        case CS_RESET:
        case CS_DESTROY:
        default:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Close: Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
    }
    return result;
}

void ATPSocket::add_data(OutgoingPacket * out_pkt, const void * buf, const size_t len){
    out_pkt->length += len;
    out_pkt->payload += len;

    out_pkt->data = reinterpret_cast<char *>(std::realloc(out_pkt->data, out_pkt->length));
    memcpy(out_pkt->data + out_pkt->length - len, buf, len);
}

ATP_PROC_RESULT ATPSocket::write(const void * buf, const size_t len){
    OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_ACK));
    add_data(out_pkt, buf, len);

    #if defined (ATP_LOG_AT_DEBUG)
        log_debug(this, "Write %u bytes to peer, seq:%u.", out_pkt->payload, seq_nr);
    #endif

    return send_packet(out_pkt);
}

ATP_PROC_RESULT ATPSocket::check_fin(OutgoingPacket * recv_pkt){
    // return >0: OK
    // return -1: error
    ATP_PROC_RESULT result = ATP_PROC_OK;
    switch(conn_state){
        case CS_UNINITIALIZED:
        case CS_IDLE:
        case CS_SYN_SENT:
        case CS_SYN_RECV:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        case CS_CONNECTED:
        case CS_CONNECTED_FULL: // B
        {
            conn_state = CS_CLOSE_WAIT;
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Recv peer's FIN, Send the last ACK to Peer, Me still alive.");
            #endif
            OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_ACK));
            send_packet(out_pkt);

            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Call ATP_CALL_ON_PEERCLOSE.");
            #endif
            atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_ON_PEERCLOSE, recv_pkt, dest_addr);
            result = invoke_callback(ATP_CALL_ON_PEERCLOSE, &arg);
            // half closed, don't send FIN immediately
            break;
        }
        case CS_FIN_WAIT_1: // A
        case CS_CLOSE_WAIT: // B
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        case CS_FIN_WAIT_2: // A
        {
            conn_state = CS_TIME_WAIT;

            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Recv peer's FIN, Send the last ACK to Peer, wait 2MSL.");
            #endif
            OutgoingPacket *  out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_ACK));
            result = send_packet(out_pkt);

            atp_callback_arguments arg;
            // arg = make_atp_callback_arguments(ATP_CALL_ON_PEERCLOSE, recv_pkt, dest_addr);
            // result = context->callbacks[ATP_CALL_ON_PEERCLOSE](&arg);
            // TODO do not destroy immediately
            arg = make_atp_callback_arguments(ATP_CALL_ON_DESTROY, recv_pkt, dest_addr);
            result = invoke_callback(ATP_CALL_ON_DESTROY, &arg);
            break;
        }
        case CS_LAST_ACK: // B
        case CS_TIME_WAIT: // A
        case CS_RESET:
        case CS_DESTROY:
        default:
        {
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Connection Error.");
            #endif
            result = ATP_PROC_ERROR;
            break;
        }
    }
    return result;
}

ATP_PROC_RESULT ATPSocket::update_myack(OutgoingPacket * recv_pkt){
    if(conn_state < CS_CONNECTED){
        // handles the last hand-shake of connection establishment

        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "Connection established on B's side, handshake completed.");
        #endif
        conn_state = CS_CONNECTED;
    }
    ATP_PROC_RESULT action = ATP_PROC_OK;
    uint16_t peer_seq = recv_pkt->get_head()->seq_nr;
    if (peer_seq <= ack_nr){
        // this packet has already been acked, DROP!
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "This is an old seq_nr:%u, my ack has already been:%u.", peer_seq, ack_nr);
        #endif
        action = ATP_PROC_DROP;
    } else if(peer_seq == ack_nr + 1){
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "This is a normal seq_nr:%u, my ack is:%u.", peer_seq, ack_nr);
        #endif
        #if defined (ATP_LOG_AT_NOTE)
            print_out(this, recv_pkt, "rcv");
        #endif
        ack_nr ++;
        action = ATP_PROC_OK;
    } else{
        // there is at least one packet not acked before this packet, so we can't ack this
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "This is an pre-arrived seq_nr:%u, my ack is still:%u.", peer_seq, ack_nr);
        #endif
        action = ATP_PROC_CACHE;
    }
    if (action == ATP_PROC_OK)
    {
        switch(conn_state){
            case CS_UNINITIALIZED:
            case CS_IDLE:
            case CS_LISTEN:
                err_sys("At state CS_UNINITIALIZED/CS_IDLE/CS_LISTEN: Ack is illegal");
                action = ATP_PROC_DROP;
                break;
            case CS_SYN_SENT:
                // already handled in `ATPSocket::process`
                err_sys("At state CS_SYN_SENT: this case is already handled in ATPSocket::process");
                action = ATP_PROC_DROP;
                break;
            case CS_SYN_RECV:
                // recv the last handshake, change state to CS_CONNECTED by update_myack automaticlly
                // connection established on side B
                // fallthrough
            case CS_CONNECTED:
            case CS_CONNECTED_FULL:
                break;
            case CS_FIN_WAIT_1: // A
                // state: A's fin is sent to B. this ack must be an ack for A's fin, 
                // if will not be a ack for previous ack, because in this case `action != ATP_PROC_OK`
                // action of ack: 
                conn_state = CS_FIN_WAIT_2;
                #if defined (ATP_LOG_AT_DEBUG)
                    log_debug(this, "Recv the ACK for my FIN from Peer, Me Die, Peer still alive.");
                #endif
                break;
            case CS_CLOSE_WAIT: // B
                // state: this is half closed state. B got A's fin and sent Ack, 
                // Now, B knew A'll not send data, but B can still send data, then A can send ack in response
                // action of ack: check that ack, because it may be an ack for B's data
                break;
            case CS_FIN_WAIT_2: // A
                // state: A is fin now, and B knew A's fin and A can't send any data bt sending Ack
                // A got B's Ack, and already switch from CS_FIN_WAIT_1 to CS_FIN_WAIT_2
                // THis should be B's FIN or B's Data
                // action of ack: discard this ack
                break;
            case CS_LAST_ACK: // B
            {
                // state: B has sent his fin, this ack must be A's response for B's fin
                // action of ack: change state
                atp_callback_arguments arg = make_atp_callback_arguments(ATP_CALL_ON_DESTROY, nullptr, dest_addr);
                action = invoke_callback(ATP_CALL_ON_DESTROY, &arg);
                conn_state = CS_DESTROY;
                #if defined (ATP_LOG_AT_DEBUG)
                    log_debug(this, "Recv the last ACK for my FIN from Peer, All Die, RIP.");
                #endif
                break;
            }
            case CS_TIME_WAIT: 
                // state, A must wait 2 * MSL and then goto CS_DESTROY
                // action of ack: simply drop
                action = ATP_PROC_DROP;
                break;
            case CS_RESET:
            case CS_DESTROY: 
                // the end
                action = ATP_PROC_DROP;
                break;
            default:
                action = ATP_PROC_DROP;
                break;
        }
    }
    return action;
}

ATP_PROC_RESULT ATPSocket::process(const ATPAddrHandle & addr, const char * buffer, size_t len){
    OutgoingPacket * recv_pkt = new OutgoingPacket();
    // set OutgoingPacket
    // must copy received message from "kernel"
    recv_pkt->data = reinterpret_cast<char *>(std::calloc(1, len));
    std::memcpy(recv_pkt->data, buffer, len);
    recv_pkt->timestamp = get_current_ms();
    ATPPacket * pkt = recv_pkt->get_head();
    recv_pkt->length = len;
    recv_pkt->payload = recv_pkt->length - sizeof(ATPPacket);

    #if defined (ATP_LOG_AT_DEBUG)
        log_debug(this, "ATPPacket recv, my_ack:%u peer_seq:%u peer_ack:%u size:%u payload:%u."
            , ack_nr, recv_pkt->get_head()->seq_nr, recv_pkt->get_head()->ack_nr, recv_pkt->length, recv_pkt->payload);
    #endif
    ATP_PROC_RESULT result = ATP_PROC_OK;
    // HANDLE IMMEDIATELY
    // SYN packet need to be handled immediately, and `addr` must register to `dest_addr` by `accept`
    // on the other hand, if we handle SYN from a queue, in `process_packet`
    // then we can't know `socket->dest_addr`
    if(pkt->get_syn() && pkt->get_ack()){
        // recv the second handshake
        // established on side A
        assert(conn_state == CS_SYN_SENT);
        conn_state = CS_CONNECTED;

        peer_sock_id = *reinterpret_cast<uint16_t *>(recv_pkt->data + sizeof(ATPPacket));
        // must set ack_nr, because now ack_nr is still 0
        ack_nr = recv_pkt->get_head()->seq_nr;
        #if defined (ATP_LOG_AT_DEBUG)
            log_debug(this, "Connection established on A's side, sending ACK immediately to B to complete handshake.");
        #endif

        #if defined (ATP_LOG_AT_NOTE)
            print_out(this, recv_pkt, "rcv");
        #endif
        // send a ack even if there's no data immediately, in order to avoid timeout
        OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_ACK));
        send_packet(out_pkt);
        result = ATP_PROC_OK;
        return result;

    } else if(pkt->get_syn()){
        // recv the first handshake
        // send the second handshake
        #if defined (ATP_LOG_AT_NOTE) 
            print_out(this, recv_pkt, "rcv");
        #endif
        this->accept(addr, recv_pkt);
        result = ATP_PROC_OK;
        return result;
    } 

    uint32_t old_ack_nr = ack_nr;
    int action = update_myack(recv_pkt);

    switch(action){
        case ATP_PROC_DROP:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Drop packet, peer_seq:%u, my ack:%u", recv_pkt->get_head()->seq_nr, ack_nr);
            #endif
            #if defined (ATP_LOG_AT_NOTE)
                print_out(this, recv_pkt, "drop");
            #endif
            delete recv_pkt;
            result = ATP_PROC_OK;
            break;
        case ATP_PROC_OK:
            result = this->receive(recv_pkt);
            break;
        case ATP_PROC_CACHE:
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Cached packet, ack:%u peer seq:%u inbuf_size: %u", ack_nr, recv_pkt->get_head()->seq_nr, inbuf.size());
            #endif
            inbuf.push(recv_pkt);
            result = ATP_PROC_OK;
            break;
        case ATP_PROC_FINISH:
            break;
    }
    if (action == ATP_PROC_OK)
    {
        // check if there is any packet which can be acked
        while(!inbuf.empty()){
            OutgoingPacket * top_packet = inbuf.top();
            action = update_myack(top_packet);
            switch(action){
                case ATP_PROC_DROP:
                    #if defined (ATP_LOG_AT_DEBUG)
                        log_debug(this, "Drop packet from cache, ack:%u peer_seq:%u", ack_nr, recv_pkt->get_head()->seq_nr);
                    #endif
                    #if defined (ATP_LOG_AT_NOTE)
                        print_out(this, top_packet, "drop");
                    #endif
                    delete top_packet;
                    result = ATP_PROC_OK;
                    break;
                case ATP_PROC_OK:
                    inbuf.pop();
                    #if defined (ATP_LOG_AT_DEBUG)
                        log_debug(this, "Process a cached ATPPacket, peer_seq:%u, my ack:%u", recv_pkt->get_head()->seq_nr, ack_nr);
                    #endif
                    result = this->receive(recv_pkt);
                    break;
                case ATP_PROC_CACHE:
                    // remain this state;
                    goto OUT_THE_LOOP;
                    result = ATP_PROC_OK;
                    break;
                case ATP_PROC_FINISH:
                    break;
            }
        }
        OUT_THE_LOOP:
            int aaa = 1;
    }
    if (ack_nr != old_ack_nr)
    {
        // if ack_nr is updated, which means I read some packets from peer
        // send an ack packet immediately
        OutgoingPacket * out_pkt = basic_send_packet(ATPPacket::create_flags(PACKETFLAG_ACK));
        send_packet(out_pkt);
    }
    if (action == ATP_PROC_FINISH)
    {
        result = action;
    } 
    else if (pkt->get_fin())
    {
        result = check_fin(recv_pkt);
    }
    // this->do_ack_packet(ack_nr);
    return result;
}

ATP_PROC_RESULT ATPSocket::invoke_callback(int callback_type, atp_callback_arguments * args){
    if (callbacks[callback_type] != nullptr)
    {
        return callbacks[callback_type](args);
    }
    #if defined (ATP_LOG_AT_DEBUG)
        log_debug(this, "An empty callback");
    #endif
    return ATP_PROC_OK;
}

ATP_PROC_RESULT ATPSocket::do_ack_packet(){
    // ack n means ack [0..n]
    while(!outbuf.empty()){
        OutgoingPacket * out_pkt = outbuf.top();
        if (out_pkt->get_head()->seq_nr <= my_seq_acked_by_peer)
        {
            #if defined (ATP_LOG_AT_DEBUG)
                log_debug(this, "Remove ATPPackct seq_nr:%u from buffer", out_pkt->get_head()->seq_nr);
            #endif
            delete out_pkt;
            outbuf.pop();
        }else{
            break;
        }
    }
}
