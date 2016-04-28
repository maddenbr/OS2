/*
 * elevator sstf (LOOK/CLOOK)
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

struct sstf_data {
	struct list_head queue;
	sector_t cur;	//current position
	int dir;	//direction
};

static inline sector_t sec_abs(sector_t a, sector_t b)
{
	if (a > b) {
		return a - b;
	}
	return b - a;
}

static void sstf_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

static int sstf_dispatch(struct request_queue *q, int force)
{
	struct sstf_data *nd = q->elevator->elevator_data; //
	struct list_head *head;
	struct request *cur;
	struct request *best;	//keeps track of the best sector to go to next
	struct request *back;	//use to check if the front and back are the same -> 1 entry in list
	sector_t bestsec;
	sector_t cursec;

	if (!list_empty(&nd->queue)) {	//if there's something to dispatch
		printk(KERN_INFO "~~~~Starting dispatch~~~~\n");
		
		best = list_entry(nd->queue.next, struct request, queuelist);
		bestsec = ULONG_MAX;	//set to the end so the first entry is better
		back = list_entry(best->queuelist.prev, struct request, queuelist);

		if (best != back) {	//if more than one element
			if (nd->dir == 1) {	//if going forward
				cur = list_entry(head, struct request, queuelist);
				cursec = blk_rq_pos(cur);
				printk(KERN_INFO "cursec = %llu \n", cursec);
				if ((sec_abs(cursec, nd->pos) < sec_abs(bestsec, nd->pos)) && (cursec > nd->pos)) {
					bestsec = cursec;
					best = cur;
					printk(KERN_INFO "new bestsec: %llu \n", bestsec);
					nd->dir = 1;
				} else {
					nd->dir = 0;
				}
			}
			if (nd->dir == 0) {	//if going backwards
				cur = list_entry(head, struct request, queuelist);
				cursec = blk_rq_pos(cur);
				printk(KERN_INFO "cursec = %llu \n", cursec);
				if ((sec_abs(cursec, nd->pos) < sec_abs(bestsec, nd->pos)) && (cursec < nd->pos)) {
					bestsec = cursec;
					best = cur;
					printk(KERN_INFO "new bestsec = %llu \n", bestsec);
					nd->dir = 0;
				} else {
					nd->dir = 1;
				}
			}
		}
		printk(KERN_INFO "bestsec = %llu, dir = %d\n", bestsec, dir);
		list_del_init(&best->queuelist);	//remove serviced requests
		nd->pos = bestsec + blk_rq_sectors(best);
		
		elv_dispatch_sort(q, best);
		printk(KERN_INFO "~~~~Dispatch ended~~~~\n");
		return 1;
	}
	//if empty, reset position and direction
	if (nd->dir == 1) {
		nd->pos = ULONG_MAX/2;
		nd->dir = 0;
	} else if (nd->dir == 0) {
		nd->pos = 0;
		nd->dir = 1;
	}
	return 0;
}

static void sstf_add_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	list_add_tail(&rq->queuelist, &nd->queue);
}

static struct request *
sstf_former_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &nd->queue) {
		return NULL;
	}
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
sstf_latter_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.next == &nd->queue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static int sstf_init_queue(struct request_queue *q)
{
	struct sstf_data *nd;
	nd = kmalloc_node(sizeof (*nd), GFP_KERNEL, q->node);
	if (!nd) {
		return NULL;
	}
	
	INIT_LIST_HEAD(&nd->queue);

	nd->dir = 1;	//set to forward, at the beginning 
	nd->pos = 0;
	return nd;
}

static void sstf_exit_queue(struct elevator_queue *e)
{
	struct noop_data *nd = e->elevator_data;

	BUG_ON(!list_empty(&nd->queue));
	kfree(nd);
}

static struct elevator_type elevator_sstf = {
	.ops = {
		.elevator_merge_req_fn		= sstf_merged_requests,
		.elevator_dispatch_fn		= sstf_dispatch,
		.elevator_add_req_fn		= sstf_add_request,
		.elevator_former_req_fn		= sstf_former_request,
		.elevator_latter_req_fn		= sstf_latter_request,
		.elevator_init_fn			= sstf_init_queue,
		.elevator_exit_fn			= sstf_exit_queue,
	},
	.elevator_name = "sstf",
	.elevator_owner = THIS_MODULE,
};

static int __init sstf_init(void)
{
	return elv_register(&elevator_sstf);
	
	return 0;
}

static void __exit sstf_exit(void)
{
	elv_unregister(&elevator_sstf);
}

module_init(sstf_init);
module_exit(sstf_exit);


MODULE_AUTHOR("Jens Axboe and Brent Madden");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSTF IO scheduler");
