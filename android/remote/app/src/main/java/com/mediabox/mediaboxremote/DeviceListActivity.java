package com.mediabox.mediaboxremote;

import android.app.ListActivity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.os.IBinder;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;

public class DeviceListActivity extends ListActivity
{
    private boolean bound = false;
    private Device[] devices;
    private ServiceConnection connection = new ServiceConnection()
    {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service)
        {
            DiscoveryService.ServiceBinder binder = (DiscoveryService.ServiceBinder) service;
            DiscoveryService discovery_service = binder.getService();
            DeviceAdapter adapter = new DeviceAdapter(DeviceListActivity.this,
                    (devices = discovery_service.getDeviceList()));
            setListAdapter(adapter);
            bound = true;
        }

        @Override
        public void onServiceDisconnected(ComponentName name)
        {
            bound = false;
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        Intent intent = new Intent(this, DiscoveryService.class);
        bindService(intent, connection, Context.BIND_AUTO_CREATE);
    }

    @Override
    public void onDestroy()
    {
        super.onDestroy();

        if (bound)
        {
            unbindService(connection);
            bound = false;
        }
    }

    @Override
    public void onListItemClick(ListView l, View v, int position, long id)
    {
        Device dev = devices[position];
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);

        Log.d("DeviceList", String.format("Device %s (%s) selected.",
                dev.name, dev.address));

        if (!prefs.getString("device", "").equals(dev.address))
        {
            SharedPreferences.Editor editor = prefs.edit();
            editor.putString("device", dev.address);
            editor.apply();

            Log.d("DeviceList", "Settings updated.");
        }
        finish();
    }

    private class DeviceAdapter extends ArrayAdapter<Device>
    {
        private class ViewHolder
        {
            private TextView itemView;
        }

        public DeviceAdapter(Context context, Device[] items)
        {
            super(context, 0, items);
        }

        public View getView(int position, View convertView, ViewGroup parent)
        {
            ViewHolder viewHolder;

            if (convertView == null)
            {
                convertView = LayoutInflater.from(this.getContext())
                        .inflate(R.layout.device_list_item, parent, false);
                viewHolder = new ViewHolder();
                viewHolder.itemView = (TextView) convertView.findViewById(R.id.label);
                convertView.setTag(viewHolder);
            }
            else
            {
                viewHolder = (ViewHolder) convertView.getTag();
            }

            Device item = getItem(position);
            if (item != null)
            {
                viewHolder.itemView.setText(String.format("%s (%s)", item.name, item.address));
            }

            return convertView;
        }
    }
}
