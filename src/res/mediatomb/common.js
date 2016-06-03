// This script will be run once before each other script is loaded. Here you
// can define any functions you want to have available in the other scripts,
// or initialisation code you want to have executed only once for each script.

/*MT_F*
    
    MediaTomb - http://www.mediatomb.cc/
    
    common.js - this file is part of MediaTomb.
    
    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    This file is free software; the copyright owners give unlimited permission
    to copy and/or redistribute it; with or without modifications, as long as
    this notice is preserved.
    
    This file is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    
    $Id: common.js 2081 2010-03-23 20:18:00Z lww $
*/

function escapeSlash(name)
{
    name = name.replace(/\\/g, "\\\\");
    name = name.replace(/\//g, "\\/");
    return name;
}

function createContainerChain(arr)
{
    var path = '';
    for (var i = 0; i < arr.length; i++)
    {
        path = path + '/' + escapeSlash(arr[i]);
    }
    return path;
}

function getYear(date)
{
    var matches = date.match(/^([0-9]{4})-/);
    if (matches)
        return matches[1];
    else
        return date;
}

function getPlaylistType(mimetype)
{
    if (mimetype == 'audio/x-mpegurl')
        return 'm3u';
    if (mimetype == 'audio/x-scpls')
        return 'pls';
    return '';
}

function getLastPath(location)
{
    var path = location.split('/');
    if ((path.length > 1) && (path[path.length - 2]))
        return path[path.length - 2];
    else
        return '';
}

function getRootPath(rootpath, location)
{
    var path = new Array();

    if (rootpath.length != '')
    {
        rootpath = rootpath.substring(0, rootpath.lastIndexOf('/'));

        var dir = location.substring(rootpath.length,location.lastIndexOf('/'));

        if (dir.charAt(0) == '/')
            dir = dir.substring(1);

        path = dir.split('/');
    }
    else
    {
        dir = getLastPath(location);
        if (dir != '')
        {
            dir = escapeSlash(dir);
            path.push(dir);
        }
    }

    return path;
}

function tranformTitle(title)
{
		title = title.replace(/\.[^/.]+$/, "");
		title = title.replace(/\t/g, " ");
		title = title.replace(/YIFY/g, "");
		title = title.replace(/BluRay/g, "");
		title = title.replace(/x264/g, "");
		title = title.replace(/BrRip/g, "");
		title = title.replace(/HDRip/g, "");
		title = title.replace(/AAC-JYK/g, "");
		title = title.replace(/bitloks/g, "");
		title = title.replace(/H264/g, "");
		title = title.replace(/AAC-RARBG/g, "");
		title = title.replace(/SiNNERS/g, "");
		title = title.replace(/X264/g, "");
		title = title.replace(/XViD-EVO/g, "");
		title = title.replace(/_/g, " ");
		//title = title.replace(/\[Dual-Audio\] \[English 5 1 \+ Hindi 2 0\]/g, "");
		title = title.replace(/psig-/g, "");
		title = title.replace(/xvid/g, "");
		title = title.replace(/dvdrip/g, "");
		title = title.replace(/ac3/g, "");
		title = title.replace(/DvDrip/g, "");
		title = title.replace(/AC3-EVO/g, "");
		title = title.replace(/internal/g, "");
		title = title.replace(/XviD/g, "");
		title = title.replace(/iNFAMOUS/g, "");
		title = title.replace(/DVDRip/g, "");
		title = title.replace(/XViD/g, "");
		title = title.replace(/HD-CAM/g, "");
		title = title.replace(/AC3-CPG/g, "");
		title = title.replace(/HQMic/g, "");
		title = title.replace(/BRRip/g, "");
		title = title.replace(/Bluray/g, "");
		title = title.replace(/500MB/g, "");
		title = title.replace(/aXXo/g, "");
		title = title.replace(/VPPV/g, "");
		title = title.replace(/BOKUTOX/g, "");
		title = title.replace(/George Lucas/g, "");
		title = title.replace(/Eng Subs/g, "");
		title = title.replace(/\./g, " ");
		var s2 = title;
		var s1;
		do {
			s1 = s2;
			s2 = title.replace(/  /g, " ")
				.replace(/--/g, "-");
		}
		while (s1 != s2);
		title = s1;

		title = title.charAt(0).toUpperCase() + title.substring(1);

		if (title == "English" ||
			title == "English-forced" ||
			title == "English-sdh" ||
			title == "Sdh" ||
			title == "Sdh-SDH" ||
			title == "Spa" ||
			title == "subs" ||
			title == "Slv")
			return "";

	return title;
}

