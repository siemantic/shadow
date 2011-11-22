/**
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2011 Rob Jansen <jansen@cs.umn.edu>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shadow.h"

void transport_free(Transport* transport) {
	MAGIC_ASSERT(transport);
	MAGIC_ASSERT(transport->vtable);

	MAGIC_CLEAR(transport);
	transport->vtable->free((Descriptor*)transport);
}

DescriptorFunctionTable transport_functions = {
	(DescriptorFreeFunc) transport_free,
	MAGIC_VALUE
};

void transport_init(Transport* transport, TransportFunctionTable* vtable, enum DescriptorType type, gint handle) {
	g_assert(transport && vtable);

	descriptor_init(&(transport->super), type, &transport_functions, handle);

	MAGIC_INIT(transport);
	MAGIC_INIT(vtable);

	transport->vtable = vtable;
	transport->protocol = type == DT_TCPSOCKET ? PTCP : type == DT_UDPSOCKET ? PUDP : PLOCAL;
	transport->inputBuffer = g_queue_new();
	transport->inputBufferSize = CONFIG_RECV_BUFFER_SIZE;
	transport->outputBuffer = g_queue_new();
	transport->outputBufferSize = CONFIG_SEND_BUFFER_SIZE;
}

gboolean transport_isBound(Transport* transport) {
	MAGIC_ASSERT(transport);
	return (transport->flags & TF_BOUND) ? TRUE : FALSE;
}

void transport_setBinding(Transport* transport, in_addr_t boundAddress, in_port_t port) {
	MAGIC_ASSERT(transport);
	g_assert(!transport_isBound(transport));
	transport->boundAddress = boundAddress;
	transport->boundPort = port;
	transport->associationKey = PROTOCOL_DEMUX_KEY(transport->protocol, port);
	transport->flags |= TF_BOUND;
}

gint transport_getAssociationKey(Transport* transport) {
	MAGIC_ASSERT(transport);
	g_assert(transport_isBound(transport));
	return transport->associationKey;
}

gboolean transport_pushInPacket(Transport* transport, Packet* packet) {
	MAGIC_ASSERT(transport);
	MAGIC_ASSERT(transport->vtable);
	return transport->vtable->process(transport, packet);
}

Packet* transport_pullOutPacket(Transport* transport) {
	return transport_removeFromOutputBuffer(transport);
}

gssize transport_sendUserData(Transport* transport, gconstpointer buffer, gsize nBytes,
		in_addr_t ip, in_port_t port) {
	MAGIC_ASSERT(transport);
	MAGIC_ASSERT(transport->vtable);
	return transport->vtable->send(transport, buffer, nBytes, ip, port);
}

gssize transport_receiveUserData(Transport* transport, gpointer buffer, gsize nBytes,
		in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(transport);
	MAGIC_ASSERT(transport->vtable);
	return transport->vtable->receive(transport, buffer, nBytes, ip, port);
}

gboolean transport_addToInputBuffer(Transport* transport, Packet* packet) {
	MAGIC_ASSERT(transport);

	/* check if the packet fits */
	guint length = packet_getPayloadLength(packet);
	if((transport->inputBufferLength + length) > transport->inputBufferSize) {
		return FALSE;
	}

	/* add to our queue */
	packet_ref(packet);
	g_queue_push_tail(transport->inputBuffer, packet);
	transport->inputBufferLength += length;

	/* we just added a packet, so we are readable */
	if(transport->inputBufferLength > 0) {
		descriptor_adjustStatus((Descriptor*)transport, DS_READABLE, TRUE);
	}

	return TRUE;
}

Packet* transport_removeFromInputBuffer(Transport* transport) {
	MAGIC_ASSERT(transport);

	/* see if we have any packets */
	Packet* packet = g_queue_pop_head(transport->inputBuffer);
	if(packet) {
		/* just removed a packet */
		guint length = packet_getPayloadLength(packet);
		transport->inputBufferLength -= length;

		/* we are not readable if we are now empty */
		if(transport->inputBufferLength <= 0) {
			descriptor_adjustStatus((Descriptor*)transport, DS_READABLE, FALSE);
		}
	}

	return packet;
}

gboolean transport_addToOutputBuffer(Transport* transport, Packet* packet) {
	MAGIC_ASSERT(transport);

	/* check if the packet fits */
	guint length = packet_getPayloadLength(packet);
	if((transport->outputBufferLength + length) > transport->outputBufferSize) {
		return FALSE;
	}

	/* add to our queue */
	g_queue_push_tail(transport->outputBuffer, packet);
	transport->outputBufferLength += length;

	/* we just added a packet, we are no longer writable if full */
	if((transport->outputBufferSize - transport->outputBufferLength) <= 0) {
		descriptor_adjustStatus((Descriptor*)transport, DS_WRITABLE, FALSE);
	}

	/* tell the interface to include us when sending out to the network */
	NetworkInterface* interface = node_lookupInterface(worker_getPrivate()->cached_node, transport->boundAddress);
	networkinterface_wantsSend(interface, transport);

	return TRUE;
}

Packet* transport_removeFromOutputBuffer(Transport* transport) {
	MAGIC_ASSERT(transport);

	/* see if we have any packets */
	Packet* packet = g_queue_pop_head(transport->outputBuffer);
	if(packet) {
		/* just removed a packet */
		guint length = packet_getPayloadLength(packet);
		transport->outputBufferLength -= length;

		/* we are writable if we now have space */
		if((transport->outputBufferSize - transport->outputBufferLength) > 0) {
			descriptor_adjustStatus((Descriptor*)transport, DS_WRITABLE, TRUE);
		}
	}

	return packet;
}
