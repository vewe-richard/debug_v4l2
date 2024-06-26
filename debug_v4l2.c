#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/videobuf2-dma-contig.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple fake V4L2 video driver");
//#define NVIDIA

struct sun6i_csi {
	struct device			*dev;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_device		v4l2_dev;
	struct media_device		media_dev;

	struct v4l2_async_notifier	notifier;

	/* video port settings */
	struct v4l2_fwnode_endpoint	v4l2_ep;

};

struct sun6i_csi_dev {
	struct sun6i_csi		csi;
	struct device			*dev;

	struct regmap			*regmap;
	struct clk			*clk_mod;
	struct clk			*clk_ram;
	struct reset_control		*rstc_bus;

	int				planar_offset[3];
};


static struct sun6i_csi_dev *_sdev;

static int fake_driver_probe(struct platform_device *pdev) {
    printk("fake platform driver probe\n");
    return 0;
}

static int fake_driver_remove(struct platform_device *pdev) {
    printk("fake remove fake driver\n");
    return 0;
}

// Fake platform driver structure
static struct platform_driver fake_platform_driver = {
    .driver = {
        .name = "fake_platform_driver",
        .owner = THIS_MODULE,
    },
    .probe = fake_driver_probe,
    .remove = fake_driver_remove,
};

// Function to create and register a fake platform device
static struct platform_device *create_fake_platform_device(void) {
    struct platform_device *pdev;
    int ret;

    struct device *existing_dev;
    struct device_driver *existing_driver;

    existing_driver = driver_find("fake_platform_driver", &platform_bus_type);
    if (existing_driver) {
         pr_info("fake_platform_driver is already registered\n");
    } else {
	ret = platform_driver_register(&fake_platform_driver);
    	if (ret){
         	pr_info("fake can't register fake_platform_driver ..\n");
        	return NULL;
	}
    	existing_driver = driver_find("fake_platform_driver", &platform_bus_type);
    }
    if (existing_driver == NULL){
         pr_info("fake can't register fake_platform_driver\n");
	 return NULL;
    }

    existing_dev = bus_find_device_by_name(&platform_bus_type, NULL, "fake_platform_device");
    if (existing_dev) {
        pr_info("fake_platform_device is already registered\n");
        put_device(existing_dev); // Drop the reference obtained by bus_find_device_by_name

        pdev = to_platform_device(existing_dev);
    	pdev->dev.driver = existing_driver;
	return pdev;
    }	    

    pdev = platform_device_alloc("fake_platform_device", -1);
    if (!pdev) {
        platform_driver_unregister(&fake_platform_driver);
        return NULL;
    }

    ret = platform_device_add(pdev);
    if (ret) {
        platform_device_put(pdev);
        platform_driver_unregister(&fake_platform_driver);
        return NULL;
    }
    pdev->dev.driver = existing_driver;
    return pdev;
}


static int sun6i_subdev_notify_complete(struct v4l2_async_notifier *notifier)
{
	pr_info("fake notify complete\n");
	return 0;
}

static const struct v4l2_async_notifier_operations sun6i_csi_async_ops = {
	.complete = sun6i_subdev_notify_complete,
};

static int (*prev_complete)(struct v4l2_async_notifier *notifier);

static int my_complete(struct v4l2_async_notifier *notifier)
{
#ifdef NVIDIA
	printk("my_complete\n");
#else
	struct sun6i_csi *csi;
	csi = container_of(notifier->v4l2_dev, struct sun6i_csi, v4l2_dev);
	printk("my_complete csi v4l2_dev name %s\n", csi->v4l2_dev.name);
	prev_complete(notifier);
#endif
	return 0;
}

static const struct v4l2_async_notifier_operations my_ops = {
	.complete = my_complete,
};


static void countNodes(struct list_head *head) {
    struct list_head *pos;
    struct v4l2_async_notifier *notifier;
    // Traverse the list and count nodes
    list_for_each(pos, head) {
	notifier = container_of(pos, struct v4l2_async_notifier, list);
	if(notifier->v4l2_dev == NULL || notifier->v4l2_dev->name == NULL){
		printk("%s:%d v4l2_dev is NULL or name is NULL\n", __FILE__, __LINE__);
		continue;
	}
#ifdef NVIDIA
	if(strstr(notifier->v4l2_dev->name, "tegra-camrtc-capture") != NULL) {
#else
	if(strstr(notifier->v4l2_dev->name, "fake") != NULL){
#endif
		printk("find the notifier\n");
	} else continue;

#ifdef NVIDIA
	//to be sure if bound and unbind is NULL
	if(notifier->ops->bound != NULL || notifier->ops->unbind != NULL){
		printk("bound and unbind not NULL, adjust the code\n");
		break;
	}
	printk("complete pointer %p\n", notifier->ops->complete);
	break;
#endif

	prev_complete = notifier->ops->complete;
	notifier->ops = &my_ops;
	notifier->ops->complete(notifier);

    }
}
static int sun6i_csi_v4l2_init(struct sun6i_csi *csi)
{
	int ret;

	v4l2_async_notifier_init(&csi->notifier);

	ret = v4l2_ctrl_handler_init(&csi->ctrl_handler, 0);
	if (ret) {
		dev_err(csi->dev, "fake V4L2 controls handler init failed (%d)\n",
			ret);
		goto clean_media;
	}

	csi->v4l2_dev.mdev = &csi->media_dev;
	csi->v4l2_dev.ctrl_handler = &csi->ctrl_handler;
	ret = v4l2_device_register(csi->dev, &csi->v4l2_dev);
	if (ret) {
		dev_err(csi->dev, "V4L2 device registration failed (%d)\n",
			ret);
		goto free_ctrl;
	}

	csi->notifier.ops = &sun6i_csi_async_ops;
	ret = v4l2_async_notifier_register(&csi->v4l2_dev, &csi->notifier);
	if (ret) {
		dev_err(csi->dev, "notifier registration failed\n");
		goto unreg_v4l2;
	}
	if(csi->notifier.list.next == NULL){
		printk("%s:%d next is NULL\n", __FILE__, __LINE__);
	} else {
		countNodes(csi->notifier.list.next);
	}
	return 0;
unreg_v4l2:
	v4l2_device_unregister(&csi->v4l2_dev);
free_ctrl:
	v4l2_ctrl_handler_free(&csi->ctrl_handler);
clean_media:
	v4l2_async_notifier_cleanup(&csi->notifier);

	return ret;
}




static int __init mymodule_init(void)
{
	//struct sun6i_csi_dev *sdev; //_sdev
	int ret;

	struct platform_device *pdev;


    	pdev = create_fake_platform_device();
	if(pdev == NULL) return -ENOMEM;


	_sdev = devm_kzalloc(&pdev->dev, sizeof(*_sdev), GFP_KERNEL);
	if (!_sdev){
        	platform_device_put(pdev);
        	platform_driver_unregister(&fake_platform_driver);
		return -ENOMEM;
	}

	_sdev->dev = &pdev->dev;

	platform_set_drvdata(pdev, _sdev);

	_sdev->csi.dev = &pdev->dev;
	ret = sun6i_csi_v4l2_init(&_sdev->csi);
	if(ret != 0){
        	platform_device_put(pdev);
        	platform_driver_unregister(&fake_platform_driver);
		kfree(_sdev);
		return ret;
	}
	return 0;
}




static void __exit mymodule_exit(void)
{
    struct sun6i_csi *csi = &_sdev->csi;
    pr_info("Exiting fake video driver\n");
    if(_sdev == NULL) return;

    v4l2_device_unregister(&csi->v4l2_dev);
    v4l2_ctrl_handler_free(&csi->ctrl_handler);
    v4l2_async_notifier_unregister(&csi->notifier);
    v4l2_async_notifier_cleanup(&csi->notifier);
    platform_driver_unregister(&fake_platform_driver);
}

module_init(mymodule_init);
module_exit(mymodule_exit);
