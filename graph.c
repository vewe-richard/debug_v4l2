/*
 * NVIDIA Media controller graph management
 *
 * Copyright (c) 2015-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Bryan Wu <pengw@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
#include <soc/tegra/chip-id.h>
#else
#include <soc/tegra/fuse.h>
#endif

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
#include <media/v4l2-fwnode.h>
#else
#include <media/v4l2-of.h>
#endif
#include <media/tegra_v4l2_camera.h>
#include <media/mc_common.h>
#include <media/csi.h>

#include "nvcsi/nvcsi.h"


MODULE_LICENSE("GPL");
/* -----------------------------------------------------------------------------
 * Graph Management
 */

static struct tegra_vi_graph_entity *
tegra_vi_graph_find_entity(struct tegra_channel *chan,
		       const struct device_node *node)
{
	struct tegra_vi_graph_entity *entity;

	list_for_each_entry(entity, &chan->entities, list) {
		if (entity->node == node)
			return entity;
	}

	return NULL;
}

static int tegra_vi_graph_build_one(struct tegra_channel *chan,
				    struct tegra_vi_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct tegra_vi_graph_entity *ent;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	struct v4l2_fwnode_link link;
#else
	struct v4l2_of_link link;
#endif
	struct device_node *ep = NULL;
	struct device_node *next;
	int ret = 0;

	if (!entity->subdev) {
		dev_err(chan->vi->dev, "%s:No subdev under entity, skip linking\n",
				__func__);
		return 0;
	}

	local = entity->entity;
	dev_dbg(chan->vi->dev, "creating links for entity %s\n", local->name);

	do {
		/* Get the next endpoint and parse its link. */
		next = of_graph_get_next_endpoint(entity->node, ep);
		if (next == NULL)
			break;

		ep = next;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		dev_dbg(chan->vi->dev, "processing endpoint %pOF\n",
				ep);
		ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
		if (ret < 0) {
			dev_err(chan->vi->dev,
			"failed to parse link for %pOF\n", ep);
			continue;
		}
#else
		dev_dbg(chan->vi->dev, "processing endpoint %s\n",
				ep->full_name);
		ret = v4l2_of_parse_link(ep, &link);
		if (ret < 0) {
			dev_err(chan->vi->dev, "failed to parse link for %s\n",
				ep->full_name);
			continue;
		}
#endif

		if (link.local_port >= local->num_pads) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			dev_err(chan->vi->dev,
				"invalid port number %u for %pOF\n",
				link.local_port, to_of_node(link.local_node));
			v4l2_fwnode_put_link(&link);
#else
			dev_err(chan->vi->dev, "invalid port number %u on %s\n",
				link.local_port, link.local_node->full_name);
			v4l2_of_put_link(&link);
#endif
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		/* Skip sink ports, they will be processed from the other end of
		 * the link.
		 */
		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			dev_dbg(chan->vi->dev, "skipping sink port %pOF:%u\n",
				to_of_node(link.local_node), link.local_port);
			v4l2_fwnode_put_link(&link);
#else
			dev_dbg(chan->vi->dev, "skipping sink port %s:%u\n",
				link.local_node->full_name, link.local_port);
			v4l2_of_put_link(&link);
#endif
			continue;
		}

		/* Skip channel entity , they will be processed separately. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		if (link.remote_node == of_fwnode_handle(chan->vi->dev->of_node)) {
			dev_dbg(chan->vi->dev, "skipping channel port %pOF:%u\n",
				to_of_node(link.local_node), link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}
#else
		if (link.remote_node == chan->vi->dev->of_node) {
			dev_dbg(chan->vi->dev, "skipping channel port %s:%u\n",
				link.local_node->full_name, link.local_port);
			v4l2_of_put_link(&link);
			continue;
		}
#endif


		/* Find the remote entity. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		ent = tegra_vi_graph_find_entity(chan, to_of_node(link.remote_node));
#else
		ent = tegra_vi_graph_find_entity(chan, link.remote_node);
#endif
		if (ent == NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			dev_err(chan->vi->dev, "no entity found for %pOF\n",
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
#else
			dev_err(chan->vi->dev, "no entity found for %s\n",
				link.remote_node->full_name);
			v4l2_of_put_link(&link);
#endif
			ret = -EINVAL;
			break;
		}

		remote = ent->entity;

		if (link.remote_port >= remote->num_pads) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			dev_err(chan->vi->dev, "invalid port number %u on %pOF\n",
				link.remote_port, to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
#else
			dev_err(chan->vi->dev, "invalid port number %u on %s\n",
				link.remote_port, link.remote_node->full_name);
			v4l2_of_put_link(&link);
#endif
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			v4l2_fwnode_put_link(&link);
#else
			v4l2_of_put_link(&link);
#endif

		/* Create the media link. */
		dev_dbg(chan->vi->dev, "creating %s:%u -> %s:%u link\n",
			local->name, local_pad->index,
			remote->name, remote_pad->index);

		ret = tegra_media_create_link(local, local_pad->index, remote,
				remote_pad->index, link_flags);
		if (ret < 0) {
			dev_err(chan->vi->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	} while (next);

	return ret;
}

static int tegra_vi_graph_build_links(struct tegra_channel *chan)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *source;
	struct media_entity *sink;
	struct media_pad *source_pad;
	struct media_pad *sink_pad;
	struct tegra_vi_graph_entity *ent;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	struct v4l2_fwnode_link link;
#else
	struct v4l2_of_link link;
#endif
	struct device_node *ep = NULL;
	int ret = 0;

	dev_dbg(chan->vi->dev, "creating links for channels\n");

	/* Device not registered */
	if (!chan->init_done)
		return -EINVAL;

	ep = chan->endpoint_node;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	dev_dbg(chan->vi->dev, "processing endpoint %pOF\n", ep);
	ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to parse link for %pOF\n",
			ep);
		return -EINVAL;
	}
#else
	dev_dbg(chan->vi->dev, "processing endpoint %s\n", ep->full_name);
	ret = v4l2_of_parse_link(ep, &link);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to parse link for %s\n",
			ep->full_name);
		return -EINVAL;
	}
#endif

	if (link.local_port >= chan->vi->num_channels) {
		dev_err(chan->vi->dev, "wrong channel number for port %u\n",
			link.local_port);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			v4l2_fwnode_put_link(&link);
#else
			v4l2_of_put_link(&link);
#endif
		return  -EINVAL;
	}

	dev_dbg(chan->vi->dev, "creating link for channel %s\n",
		chan->video->name);

	/* Find the remote entity. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	ent = tegra_vi_graph_find_entity(chan, to_of_node(link.remote_node));
	if (ent == NULL) {
		dev_err(chan->vi->dev, "no entity found for %pOF\n",
			to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
		return -EINVAL;
	}
#else
	ent = tegra_vi_graph_find_entity(chan, link.remote_node);
	if (ent == NULL) {
		dev_err(chan->vi->dev, "no entity found for %s\n",
			link.remote_node->full_name);
			v4l2_of_put_link(&link);
		return -EINVAL;
	}
#endif

	if (ent->entity == NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		dev_err(chan->vi->dev, "entity not bounded %pOF\n",
			to_of_node(link.remote_node));
		v4l2_fwnode_put_link(&link);
#else
		dev_err(chan->vi->dev, "entity not bounded %s\n",
			link.remote_node->full_name);
		v4l2_of_put_link(&link);
#endif
		return -EINVAL;
	}

	source = ent->entity;
	source_pad = &source->pads[link.remote_port];
	sink = &chan->video->entity;
	sink_pad = &chan->pad;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
			v4l2_fwnode_put_link(&link);
#else
			v4l2_of_put_link(&link);
#endif

	/* Create the media link. */
	dev_dbg(chan->vi->dev, "creating %s:%u -> %s:%u link\n",
		source->name, source_pad->index,
		sink->name, sink_pad->index);

	ret = tegra_media_create_link(source, source_pad->index,
				       sink, sink_pad->index,
				       link_flags);
	if (ret < 0) {
		dev_err(chan->vi->dev,
			"failed to create %s:%u -> %s:%u link\n",
			source->name, source_pad->index,
			sink->name, sink_pad->index);
		return -EINVAL;
	}

	ret = tegra_channel_init_subdevices(chan);
	if (ret < 0) {
		dev_err(chan->vi->dev, "Failed to initialize sub-devices\n");
		return -EINVAL;
	}

	return 0;
}



int my_tegra_vi_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct tegra_channel *chan =
		container_of(notifier, struct tegra_channel, notifier);
	struct tegra_vi_graph_entity *entity;
	int ret;

	dev_dbg(chan->vi->dev, "notify complete, all subdevs registered\n");

	/* Allocate video_device */
	ret = tegra_channel_init_video(chan);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to allocate video device %s\n",
			chan->video->name);
		return ret;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	ret = video_register_device(chan->video, VFL_TYPE_GRABBER, -1);
#else
	ret = video_register_device(chan->video, VFL_TYPE_VIDEO, -1);
#endif
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to register %s\n",
			chan->video->name);
		goto register_device_error;
	}

	/* Create links for every entity. */
	list_for_each_entry(entity, &chan->entities, list) {
		if (entity->entity != NULL) {
			ret = tegra_vi_graph_build_one(chan, entity);
			if (ret < 0)
				goto graph_error;
		}
	}

	/* Create links for channels */
	ret = tegra_vi_graph_build_links(chan);
	if (ret < 0)
		goto graph_error;

	ret = v4l2_device_register_subdev_nodes(&chan->vi->v4l2_dev);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to register subdev nodes\n");
		goto graph_error;
	}

	chan->link_status++;

	return 0;

graph_error:
	video_unregister_device(chan->video);
register_device_error:
	video_device_release(chan->video);

	return ret;
}


