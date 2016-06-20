package com.yahoo.vespa.hosted.node.admin.integrationTests;

import com.yahoo.vespa.applicationmodel.HostName;
import com.yahoo.vespa.hosted.node.admin.ContainerNodeSpec;
import com.yahoo.vespa.hosted.node.admin.NodeAdmin;

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Mock with some simple logic
 * @autor dybis
 */
public class NodeAdminMock implements NodeAdmin {

    Set<HostName> hostnames = new HashSet<>();

    boolean freezeSetState = false;
    public AtomicBoolean frozen = new AtomicBoolean(false);

    // We make it threadsafe as the test have its own peeking thread.
    private Object monitor = new Object();

    @Override
    public void setState(List<ContainerNodeSpec> containersToRun) {
        synchronized (monitor) {
            hostnames.clear();
            containersToRun.forEach(container -> hostnames.add(container.hostname));
        }
    }

    @Override
    public boolean setFreezeAndCheckIfAllFrozen(boolean freeze) {
        freezeSetState = freeze;
        return frozen.get();
    }

    @Override
    public Set<HostName> getListOfHosts() {
        synchronized (monitor) {
            return hostnames;
        }
    }
}
