import json
import pytest
# import time
import os
# import uuid

import grpc
import nextapp_pb2
import nextapp_pb2_grpc

@pytest.fixture(scope = 'module')
def gd():
    channel = grpc.insecure_channel(os.getenv('NA_GRPC', '127.0.0.1:10321'))
    stub = nextapp_pb2_grpc.NextappStub(channel)
    return {'stub': stub}

def test_add_root_node(gd):
    node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name='first')
    req = nextapp_pb2.CreateNodeReq(node=node)
    status = gd['stub'].CreateNode(req)
    assert status.error == nextapp_pb2.Error.OK
    assert status.node.name == 'first'

def test_add_child_node(gd):
    node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name='second')
    req = nextapp_pb2.CreateNodeReq(node=node)
    status = gd['stub'].CreateNode(req)
    assert status.error == nextapp_pb2.Error.OK
    assert status.node.name == 'second'

    parent = status.node.uuid
    assert parent != ""
    print ("parent is {}".format(parent))
    node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name='child-of-second', parent=parent)
    req = nextapp_pb2.CreateNodeReq(node=node)
    status = gd['stub'].CreateNode(req)
    assert status.error == nextapp_pb2.Error.OK
    assert status.node.name == 'child-of-second'
    assert status.node.parent == parent

def test_add_child_tree(gd):
    name = "third"
    node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name=name)
    req = nextapp_pb2.CreateNodeReq(node=node)
    status = gd['stub'].CreateNode(req)
    assert status.error == nextapp_pb2.Error.OK
    assert status.node.name == name
    parent = status.node.uuid
    assert parent != ""

    for i in range(20):
        name = "third-{}".format(i)
        node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name=name, parent=parent)
        req = nextapp_pb2.CreateNodeReq(node=node)
        status = gd['stub'].CreateNode(req)
        assert status.error == nextapp_pb2.Error.OK
        assert status.node.name == name
        assert status.node.parent == parent

        iiparent = status.node.uuid
        for ii in range(8):
            name = "third-{}-{}".format(i, ii)
            node = nextapp_pb2.Node(kind=nextapp_pb2.Node.Kind.FOLDER, name=name, parent=iiparent)
            req = nextapp_pb2.CreateNodeReq(node=node)
            status = gd['stub'].CreateNode(req)
            assert status.error == nextapp_pb2.Error.OK
            assert status.node.name == name
            assert status.node.parent == iiparent


def test_add_tenant(gd):
    template = nextapp_pb2.Tenant(kind=nextapp_pb2.Tenant.Kind.Regular, name='dogs')
    req = nextapp_pb2.CreateTenantReq(tenant=template)
    status = gd['stub'].CreateTenant(req)
    assert status.error == nextapp_pb2.Error.OK


def test_add_tenant_with_user(gd):
    template = nextapp_pb2.Tenant(kind=nextapp_pb2.Tenant.Kind.Regular, name='cats')
    req = nextapp_pb2.CreateTenantReq(tenant=template)
    req.users.extend([nextapp_pb2.User(kind=nextapp_pb2.User.Kind.Regular, name='kitty', email='kitty@example.com')])

    status = gd['stub'].CreateTenant(req)
    assert status.error == nextapp_pb2.Error.OK

    # Todo. Fetch the user to validate

def test_get_nodes(gd):
    req = nextapp_pb2.GetNodesReq()
    nodes = gd['stub'].GetNodes(req)


